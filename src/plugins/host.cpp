// The plugin host. One plugin thread (4 MB stack) runs a loop over an inbox of commands (from the
// screen) and completions (of host work, from any thread), the plugins' timers, and the budgets of
// the calls in flight.
//
// Calls. Everything that runs a plugin's code is a *call* with a budget counted from when it
// starts: loading main.js, an event handler, the background task, a timer callback whose call has
// ended. A call is in flight until what it returned settles (calls interleave at `await`). Its
// code runs in *entries* — the call itself, then each continuation: a completed piece of host work
// it started, or a timer it set — each entry under the call's deadline (Runtime::setDeadline),
// followed by the promise jobs it queued. An entry is attributed to the call that started the
// work or the timer; continuations of a call that already settled run as a short call of their own
// (the event budget). Waiting for the user (askUser) pauses the call's clock; code of the call that
// runs meanwhile (a timer, a continuation) still counts — only waiting is free. A call asks at most
// kMaxQuestions times.
//
// Quiet run time. Outside user-caused calls and the background run, a plugin's code may run at
// most deps.quietRunMs in any deps.quietWindowMs (2 s a minute). Past that, a timer or continuation
// whose call has ended waits until the window allows it (not failed, not counted): a timer chain
// can't keep the CPU busy or poll the network all day. The plugin thread and its workers also run
// at a lower priority (lowerThreadPriority).
//
// Stopping. When a call's deadline passes — while its code runs (the interrupt) or while it waits
// (checked by the loop) — or it runs out of memory (its own 32 MB, or the 96 MB pool all plugins
// share) or stack, it is stopped: its timers are dropped, its questions withdrawn, and its pending
// host work cancelled. That work's promises are rejected with zk.LimitError in a Cleanup call of
// their own, posted to the inbox (never nested, so the loop runs in between), so whatever awaited
// them moves on; code running in a Cleanup can't start host work or timers (rejected at once). A
// stop and an uncaught error count alike, a stopped Cleanup too: three in a row turn the plugin
// off; a successful call resets the count. A stop that cut into running code restarts the plugin
// (fresh runtime, main.js again): quickjs-ng leaves an interrupted async function's promise
// pending forever, which would hang anything chained on it.
//
// No C++ exception crosses into QuickJS: every native is defined through guardedNative
// (plugin_ctx.h), which turns std::bad_alloc into the memory limit and anything else into a JS
// Error. Host code around the natives that still throws into an entry poisons it: the call is
// dropped with its timers, questions and work, counted, and the plugin restarts. Every task, timer
// pass and budget check also runs behind an exception barrier that logs to the app log.
//
// Crash marker. While a plugin's code runs, <crashMarker> holds its id (written and synced before
// its entry, rewritten only when another plugin's code runs next, removed before the thread
// waits). If the app dies with it there, the next start turns that plugin off.

#include "plugins/host.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

#include "core/wake.h"
#include "plugins/plugin_ctx.h"
#include "plugins/plugin_http.h"
#include "plugins/plugin_log.h"
#include "plugins/state.h"

namespace zk {

namespace fs = std::filesystem;
using plugins::LogLevel;
using plugins::PluginCtx;

namespace plugins {

using Task = std::function<void(PluginHost::Impl&)>;

struct Inbox {
    std::mutex mu;
    std::condition_variable wake;  // the plugin thread
    std::condition_variable idle;  // waitIdle()
    std::deque<Task> tasks;
    bool stopping = false, closed = false;
    bool busy = false;  // working through tasks, or calls are in flight

    bool post(Task t) {
        std::lock_guard<std::mutex> lock(mu);
        if (closed) return false;
        tasks.push_back(std::move(t));
        wake.notify_one();
        return true;
    }
};

// Render: a screen's render (its success is the tree's: an invalid tree is a failure).
enum class CallKind { Load, Event, Background, Timer, Continued, Cleanup, Render };

using Opens = std::vector<std::pair<std::string, std::optional<nlohmann::json>>>;  // zk.ui.open: screen id, params
struct Call;
// Runs when a call ends (succeeded: `text` is the string it resolved with; failed or stopped: the
// message), on the plugin thread, after its code; must not run JavaScript (post a task for that).
using CallDone = std::function<void(Call&, bool ok, const std::string& text)>;

struct Call {
    uint64_t id = 0;
    CallKind kind = CallKind::Event;
    std::string what;  // "the syncFinished handler"
    int budgetMs = 0;
    int64_t startMs = 0, pausedMs = 0, pausedSince = 0;
    int pauses = 0;
    int64_t ranWhilePausedMs = 0;  // only waiting is free: its entries' run time during a pause still counts
    int questions = 0;             // asked so far (at most kMaxQuestions)
    bool userCaused = false;
    bool quietFree = false;  // not counted as quiet run time though not user-caused (a render answering the user)
    bool resets = true;     // success clears the failure count (not a restart's reload)
    std::string stopLimit, stopText;  // a cleanup: why its call was stopped
    bool returned = false;  // its function has returned; `promise` holds what it returned, if a promise
    js::Value promise;
    js::Value value;  // what it returned when not a promise (kept only when `done` is set)
    // Screens: the instance whose onEvent this is (zk.ui.open and close allowed), or an action's
    // run (zk.ui.open allowed); what they asked for, applied when the call ends.
    uint64_t screen = 0;
    bool action = false;
    uint64_t actionRun = 0;  // an action's run: PluginHost::runAction's number
    Opens opens;
    bool closes = false;
    std::optional<nlohmann::json> closeResult;
    CallDone done;
    int64_t deadline(int64_t now) const {
        return startMs + budgetMs + pausedMs + (pauses ? now - pausedSince : 0) - ranWhilePausedMs;
    }
};

struct Timer {
    int64_t dueMs = 0;
    uint64_t owner = 0;
    std::string ownerWhat;
    js::Value callback;
};

struct PendingWork {
    uint64_t owner = 0;
    std::string ownerWhat;
    js::Value resolve, reject;
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct Doomed {  // work of a stopped call, rejected with LimitError in a cleanup of its own
    PendingWork work;
    std::string limit, text;
};

struct Ask {
    uint64_t owner = 0;
    std::function<void(bool)> answered;
};

struct PluginRun {
    uint64_t serial = 0;
    std::map<uint64_t, Call> calls;  // in flight
    uint64_t current = 0;            // the call whose entry is running
    std::map<std::string, std::vector<js::Value>> handlers;
    js::Value background;
    struct Entry {
        PluginEntry info;
        js::Value fn;       // run / render
        js::Value onEvent;  // a screen's (undefined if none)
    };
    std::vector<Entry> entries;
    std::map<uint64_t, Timer> timers;
    std::map<uint64_t, PendingWork> work;
    std::vector<Doomed> doomed;
    std::map<uint64_t, Ask> asks;  // by request id
    std::vector<std::pair<void*, js::Value>> unhandled;  // rejected promises nobody handled yet
    bool loadFailed = false;
    // The exact error objects the host rejected zk.fetch calls with for a lost connection (the last
    // few, kept alive so identity means something). Only these are "the connection's fault".
    std::vector<js::Value> networkErrors;
    // Quiet run time (PluginHostDeps::quietRunMs): (end, ms) of each entry outside user-caused calls
    // and the background run in the last window, and continuations of ended calls that wait for it.
    std::deque<std::pair<int64_t, int64_t>> quietRuns;
    struct Waiting {
        int64_t dueMs = 0;
        std::string what;
        std::function<void()> body;
    };
    std::deque<Waiting> waiting;
    bool throttled = false;  // logged once until an ended call's code runs again
    // Open screens, by instance. One onEvent or render of a screen runs at a time (`busy`); what
    // comes meanwhile waits in `queue` (second: sent by the app, so counted in ScreenTree.requestId).
    struct OpenScreen {
        std::string screenId;
        std::optional<nlohmann::json> params;
        uint64_t parent = 0;
        std::deque<std::pair<nlohmann::json, bool>> queue;
        bool busy = false, counting = false;
        bool closed = false;      // left or closed by the plugin: no more renders; gone once its queue is empty
        bool failed = false;      // ScreenError sent: nothing more runs for it
        bool refreshDue = false;  // zk.ui.refresh while busy
        uint64_t received = 0, handled = 0;
    };
    std::map<uint64_t, OpenScreen> screens;
};

}  // namespace plugins

using namespace plugins;

namespace {

int64_t steadyMs() { return js::Runtime::nowMs(); }

// Screen instances, unique in the process (a host made again after an update's install never
// reuses one the app still holds); openScreen takes one on the screen thread.
std::atomic<uint64_t> gNextInstance{0};
// Action runs (PluginHost::runAction), numbered on the screen thread.
std::atomic<uint64_t> gNextRun{0};

[[maybe_unused]] constexpr int kPluginNice = 10;

int64_t systemMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string seconds(int ms) {
    std::ostringstream s;
    if (ms % 1000 == 0) s << ms / 1000;
    else {
        s << ms / 1000.0;
    }
    return s.str() + " s";
}

std::string capitalised(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

bool readFile(const fs::path& p, std::string& out, size_t cap) {
    std::error_code ec;
    auto size = fs::file_size(p, ec);
    if (ec || size > cap) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string jsString(JSContext* c, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) {
        JS_FreeValue(c, JS_GetException(c));
        return {};
    }
    std::string out(s, len);
    JS_FreeCString(c, s);
    return out;
}

JSModuleDef* refuseImport(JSContext* c, const char* name, void*) {
    JS_ThrowReferenceError(c, "main.js imports \"%s\", but a plugin is one file: bundle everything into one main.js "
                              "(the SDK's build does this)", name);
    return nullptr;
}

void trackRejection(JSContext*, JSValueConst promise, JSValueConst reason, bool handled, void* opaque) {
    auto* p = static_cast<PluginCtx*>(opaque);
    if (!p->run) return;
    auto& list = p->run->unhandled;
    void* key = JS_VALUE_GET_PTR(promise);
    if (handled) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](auto& e) { return e.first == key; }), list.end());
    } else {
        list.emplace_back(key, js::Value(p->cx.raw(), JS_DupValue(p->cx.raw(), reason)));
    }
}

}  // namespace

// ---- The host -------------------------------------------------------------------------------

struct PluginHost::Impl {
    // Cancel: cut short on purpose (sleep), Closed: the user closed what waited for it; neither a failure.
    enum class Limit { Time, Memory, Pool, Stack, Cancel, Closed };

    PluginHostDeps deps;
    PluginState state;
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    pthread_t thread{};
    bool started = false, joined = false;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> version{0};  // entriesVersion()

    // Shared with the screen thread.
    mutable std::mutex mu;  // folders, loadedIds, problems, events
    std::vector<PluginFolder> folders;
    std::set<std::string> loadedIds;
    std::map<std::string, std::string> problems;  // a current problem, shown on its row
    std::set<std::string> held;                    // its files are being swapped: not loaded (hold/release)
    std::map<std::string, std::string> withdrawn;  // id → why the catalog withdrew it
    std::deque<PluginHostEvent> events;
    std::string webdavHost;  // zk.fetch refuses it; the app may change it (setWebdavHost)
    std::mutex runningMu;
    js::Runtime* running = nullptr;  // whose code is running (stop() interrupts it)

    // The plugin thread's own (and the constructor's, before the thread starts).
    js::MemoryPool pool;  // all plugins' JavaScript memory (before `loaded`: outlives the runtimes)
    std::map<std::string, std::unique_ptr<PluginCtx>> loaded;
    std::map<std::string, std::unique_ptr<PluginLog>> logs;
    std::set<std::string> failedLoad;  // tried since the last reload and failed
    std::set<std::string> turnOffs;    // three failures in a row: off at the next safe point
    std::set<std::string> restarts;    // stopped mid-run: a fresh runtime at the next safe point
    bool backgroundOpen = false;       // a runBackground() whose tasks haven't all ended: BackgroundDone is due
    bool appInBackground = false;      // setBackground(): timers of ended calls wait for the foreground
    uint64_t backgroundRound = 0;      // the round that Done reports (the latest asked for)
    std::map<std::string, int> uncounted;  // consecutive background runs not counted for a lost connection
    std::map<std::string, std::vector<PluginEntry>> entryList;  // loaded plugins' entry points (under mu)
    std::set<std::string> withTask;    // loaded plugins that registered a background task (under mu)
    uint64_t nextSerial = 0, nextCall = 0, nextWork = 0, nextTimer = 0, nextRequest = 0;
    std::string markerId;  // what the crash marker says now ("" = no marker)
    // zk.fetch's workers: made on the plugin thread at the first fetch; shut down by stop().
    std::unique_ptr<plugins::PluginNet> net;
    // zk.library's connection and identifier index, made at the first library call (bindings_library.cpp).
    std::shared_ptr<void> library;

    explicit Impl(PluginHostDeps d) : deps(std::move(d)), state(deps.pluginsDir + "/.state.json") {
        webdavHost = deps.webdavHost;
        if (!deps.clock) deps.clock = systemMs;
        pool.limit = deps.memoryTotal;
        folders = scanPlugins(deps.pluginsDir, deps.appVersion);
        checkCrashMarker();
    }

    // ---- Small helpers --------------------------------------------------------------------

    std::string nameOf(const std::string& id) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& f : folders)
            if (f.dir == id && f.manifest) return f.manifest->name;
        return id;
    }

    void log(const std::string& id, LogLevel level, const std::string& text) {
        auto& l = logs[id];
        if (!l) {
            std::error_code ec;
            if (!fs::is_directory(fs::path(deps.pluginsDir) / id, ec)) return;  // never recreate a removed folder
            l = std::make_unique<PluginLog>((fs::path(deps.pluginsDir) / id / "plugin.log").string(),
                                            PluginLog::kMaxBytes, deps.clock);
        }
        std::string line = l->write(level, text);
        if (deps.logSink) deps.logSink(id, line);
    }

    void postEvent(PluginHostEvent ev) {
        if (ev.kind == PluginHostEvent::Kind::Badge) ++version;
        {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(std::move(ev));
        }
        wakeUi();  // the loop routes the host's events only when it runs
    }

    // Drops the plugin's questions owned by `callId` (0: all of them) and tells the screen, which
    // takes them away unanswered.
    void withdrawAsks(PluginCtx& p, uint64_t callId) {
        auto& asks = p.run->asks;
        for (auto it = asks.begin(); it != asks.end();) {
            if (callId != 0 && it->second.owner != callId) {
                ++it;
                continue;
            }
            postEvent({PluginHostEvent::Kind::Withdrawn, p.manifest.id, p.manifest.name, "", {}, it->first, ""});
            it = asks.erase(it);
        }
    }

    void setLoaded(const std::string& id, bool on) {
        ++version;
        {
            std::lock_guard<std::mutex> lock(mu);
            entryList.erase(id);  // a fresh load registers again
            if (on) loadedIds.insert(id);
            else {
                loadedIds.erase(id);
                withTask.erase(id);
            }
        }
        wakeUi();  // screens showing plugins look at the version
    }

    void setProblem(const std::string& id, std::string problem) {
        std::lock_guard<std::mutex> lock(mu);
        if (problem.empty()) problems.erase(id);
        else problems[id] = std::move(problem);
    }

    // ---- Crash marker -----------------------------------------------------------------------

    void checkCrashMarker() {
        if (deps.crashMarker.empty()) return;
        std::string id;
        if (!readFile(deps.crashMarker, id, 4096)) return;
        std::error_code ec;
        fs::remove(deps.crashMarker, ec);
        while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back()))) id.pop_back();
        bool known = false;
        for (const auto& f : folders) known = known || f.dir == id;
        if (!known) return;
        const std::string name = nameOf(id);
        const std::string text = "The app stopped while " + name + " was running — turned off";
        state.recordFailure(id, text);
        state.stop(id);
        log(id, LogLevel::Error, text);
        postEvent({PluginHostEvent::Kind::Disabled, id, name, text, {}, 0, ""});
    }

    void writeMarker(const std::string& id) {
        if (deps.crashMarker.empty() || markerId == id) return;
        int fd = ::open(deps.crashMarker.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return;
        ssize_t n = ::write(fd, id.data(), id.size());
        (void)n;
        ::fsync(fd);
        ::close(fd);
        markerId = id;
    }

    void clearMarker() {
        if (markerId.empty()) return;
        ::unlink(deps.crashMarker.c_str());
        markerId.clear();
    }

    // ---- The loop ----------------------------------------------------------------------------

    void loop() {
        for (;;) {
            std::deque<Task> batch;
            const int64_t wakeAt = nextWakeMs();
            const bool inFlight = anyInFlight();
            {
                std::unique_lock<std::mutex> lock(inbox->mu);
                if (inbox->tasks.empty() && !inbox->stopping) {
                    lock.unlock();
                    clearMarker();  // no plugin code runs while the thread waits
                    lock.lock();
                }
                if (inbox->tasks.empty() && !inbox->stopping) {
                    inbox->busy = inFlight;
                    if (!inFlight) inbox->idle.notify_all();
                    auto ready = [&] { return !inbox->tasks.empty() || inbox->stopping; };
                    if (wakeAt == 0) inbox->wake.wait(lock, ready);
                    else inbox->wake.wait_until(lock, std::chrono::steady_clock::time_point(std::chrono::milliseconds(wakeAt)), ready);
                }
                if (inbox->stopping) break;
                batch.swap(inbox->tasks);
                inbox->busy = true;
            }
            for (auto& task : batch) {
                if (stopping) break;
                guarded("a command", [&] { task(*this); });
                guarded("a turn-off", [&] { applyTurnOffs(); });
            }
            if (stopping) break;
            guarded("the timers", [&] { fireTimers(); });
            guarded("a turn-off", [&] { applyTurnOffs(); });
            guarded("the budgets", [&] { checkDeadlines(); });
            guarded("a turn-off", [&] { applyTurnOffs(); });
            guarded("the background round", [&] { checkBackgroundRound(); });
        }
        while (!loaded.empty()) unload(loaded.begin()->first);
        clearMarker();
        std::lock_guard<std::mutex> lock(inbox->mu);
        inbox->closed = true;
        inbox->busy = false;
        inbox->tasks.clear();
        inbox->idle.notify_all();
    }

    // An exception barrier: a C++ exception on the plugin thread is logged, never fatal.
    void guarded(const char* what, const std::function<void()>& f) {
        try {
            f();
        } catch (const std::exception& e) {
            hostLog(std::string("plugins: an internal error in ") + what + ": " + e.what());
        } catch (...) {
            hostLog(std::string("plugins: an unknown internal error in ") + what);
        }
    }

    bool anyInFlight() const {
        for (const auto& [id, p] : loaded)
            if (!p->run->calls.empty()) return true;
        return false;
    }

    // A timer whose call has ended (it would run as a call of its own) while the app is in the
    // background: it waits, not woken for, until the foreground.
    bool timerPaused(const PluginCtx& p, const Timer& t) const { return appInBackground && !p.run->calls.count(t.owner); }

    int64_t nextWakeMs() const {
        int64_t at = 0;
        auto earliest = [&](int64_t t) { at = at == 0 ? t : std::min(at, t); };
        const int64_t now = steadyMs();
        for (const auto& [id, p] : loaded) {
            for (const auto& [tid, t] : p->run->timers)
                if (!timerPaused(*p, t)) earliest(t.dueMs);
            for (const auto& w : p->run->waiting) earliest(w.dueMs);
            for (const auto& [cid, c] : p->run->calls)
                if (!c.pauses) earliest(c.deadline(now) + 1);
        }
        return at;
    }

    // ---- Loading -----------------------------------------------------------------------------

    std::vector<PluginFolder> foldersCopy() {
        std::lock_guard<std::mutex> lock(mu);
        return folders;
    }

    // A call is due: every enabled plugin that isn't loaded yet loads now.
    void activate() {
        for (const auto& f : foldersCopy()) {
            if (!f.problem.empty() || !f.manifest || loaded.count(f.dir) || failedLoad.count(f.dir) || turnOffs.count(f.dir))
                continue;
            if (!state.enabled(f.dir)) continue;
            load(f);
            applyTurnOffs();
        }
    }

    // `afresh`: a restart after a stop, whose success doesn't clear the failure that caused it.
    void load(const PluginFolder& f, bool afresh = false) {
        const std::string& id = f.dir;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (held.count(id) || withdrawn.count(id)) return;
        }
        setProblem(id, "");
        const fs::path dir = fs::path(deps.pluginsDir) / id;
        std::string source;
        if (!readFile(dir / f.manifest->main, source, 16u << 20)) {
            failedLoad.insert(id);
            recordFailure(id, f.manifest->main + " can't be read (missing, or over 16 MB)");
            return;
        }

        // Under pool pressure (other plugins' memory) the runtime itself may not fit: that is this
        // plugin's load failing, not a crash on the plugin thread.
        std::unique_ptr<PluginCtx> owned;
        try {
            owned = std::make_unique<PluginCtx>(*this, *f.manifest, dir.string(), deps.memoryPerPlugin);
        } catch (const std::exception& e) {
            log(id, LogLevel::Error, std::string("The app couldn't load this plugin: ") + e.what());
            failedLoad.insert(id);
            setProblem(id, "The app couldn't load this plugin: not enough plugin memory");
            return;
        }
        PluginCtx& p = *owned;
        p.run->serial = ++nextSerial;
        std::string settingsText;
        if (readFile(dir / "settings.json", settingsText, 1u << 20)) {
            auto j = nlohmann::json::parse(settingsText, nullptr, false);
            if (j.is_object()) p.settings = std::move(j);
            else log(id, LogLevel::Warn, "settings.json isn't a JSON object; zk.settings is {}");
        }
        JSContext* c = p.cx.raw();
        JS_SetContextOpaque(c, &p);
        JS_SetModuleLoaderFunc(p.rt.raw(), nullptr, refuseImport, nullptr);
        JS_SetHostPromiseRejectionTracker(p.rt.raw(), trackRejection, &p);
        p.zk = js::Value(c, JS_NewObject(c));
        JSValue global = JS_GetGlobalObject(c);
        JS_SetPropertyStr(c, global, "zk", JS_DupValue(c, p.zk.raw()));
        JS_FreeValue(c, global);
        try {
            installCore(p);
            installStorage(p);
            installFetch(p);
            installLibrary(p);
            installZotero(p);
            installEntry(p);
            installUi(p);
            for (auto& install : deps.extraBindings) install(p);
        } catch (const std::exception& e) {
            log(id, LogLevel::Error, std::string("The app couldn't prepare this plugin: ") + e.what());
            failedLoad.insert(id);
            setProblem(id, "The app couldn't prepare this plugin (see its log)");
            return;
        }

        loaded[id] = std::move(owned);
        setLoaded(id, true);
        startCall(p, CallKind::Load, "loading " + f.manifest->main, deps.handlerBudgetMs, false,
                  [&] {
                      p.cx.evalModule(source, f.manifest->main);
                      return js::Value();
                  },
                  !afresh);
        if (p.run->loadFailed) {
            failedLoad.insert(id);
            unload(id);
        }
    }

    void resetData(const std::string& id) {
        bool known = false;
        for (const auto& f : foldersCopy()) known = known || f.dir == id;
        if (!known) return;
        auto it = loaded.find(id);
        resetPluginData((fs::path(deps.pluginsDir) / id).string(), it == loaded.end() ? nullptr : it->second.get());
    }

    void unload(const std::string& id) {
        auto it = loaded.find(id);
        if (it == loaded.end()) return;
        for (auto& [wid, w] : it->second->run->work) w.cancel->store(true);
        withdrawAsks(*it->second, 0);
        if (!stopping) {  // turned off, restarted, reloaded: its open screens can't be shown any more
            for (const auto& [instance, sc] : it->second->run->screens)
                if (!sc.closed && !sc.failed) postScreen(*it->second, PluginHostEvent::Kind::ScreenError, instance, cantShow(*it->second));
            for (const auto& [cid, c] : it->second->run->calls)  // and its actions still running won't answer
                if (c.actionRun && c.done)
                    postAction(PluginHostEvent::Kind::ActionError, id, it->second->manifest.name, c.actionRun,
                               it->second->manifest.name + " couldn't do that: it stopped");
        }
        if (net) net->sweep();  // its queued fetches (and their bodies) go now
        loaded.erase(it);  // the runtime and everything in it
        setLoaded(id, false);
    }

    // The safe point (no plugin code running): turn off, then restart, whoever asked for it.
    void applyTurnOffs() {
        while (!turnOffs.empty() || !restarts.empty()) {
            if (turnOffs.empty()) {
                const std::string id = *restarts.begin();
                restarts.erase(restarts.begin());
                if (!loaded.count(id) || stopping) continue;
                unload(id);
                for (const auto& f : foldersCopy())
                    if (f.dir == id && f.manifest && f.problem.empty() && state.enabled(id)) {
                        log(id, LogLevel::Warn, nameOf(id) + " restarted: its code was stopped mid-run, so main.js runs afresh "
                                                "(module variables start over; zk.storage keeps what it stored)");
                        load(f, true);
                    }
                continue;
            }
            const std::string id = *turnOffs.begin();
            turnOffs.erase(turnOffs.begin());
            restarts.erase(id);
            unload(id);
            state.stop(id);
            ++version;
            const std::string name = nameOf(id);
            log(id, LogLevel::Error, name + " stopped working — turned off after 3 failures in a row");
            postEvent({PluginHostEvent::Kind::Disabled, id, name, name + " stopped working — turned off", {}, 0, ""});
        }
    }

    // ---- Calls and entries -------------------------------------------------------------------

    PluginCtx* bySerial(uint64_t serial) {
        for (auto& [id, p] : loaded)
            if (p->run->serial == serial) return p.get();
        return nullptr;
    }

    Call* findCall(PluginCtx& p, uint64_t id) {
        auto it = p.run->calls.find(id);
        return it == p.run->calls.end() ? nullptr : &it->second;
    }

    // Starts a call: `body` runs its function and returns what it returned.
    void startCall(PluginCtx& p, CallKind kind, std::string what, int budgetMs, bool userCaused,
                   const std::function<js::Value()>& body, bool resets = true, const std::function<void(Call&)>& setup = {}) {
        if (stopping) return;
        Call c;
        c.resets = resets;
        c.id = ++nextCall;
        c.kind = kind;
        c.what = std::move(what);
        c.budgetMs = budgetMs;
        c.startMs = steadyMs();
        c.userCaused = userCaused;
        if (setup) setup(c);
        const uint64_t id = c.id;
        p.run->calls.emplace(id, std::move(c));
        runEntry(p, id, [&] {
            js::Value r = body();
            if (Call* call = findCall(p, id)) {
                call->returned = true;
                if (r.isPromise()) call->promise = std::move(r);
                else if (call->done) call->value = std::move(r);
            }
        });
    }

    // Runs `body` (and then the promise jobs it queued) as part of call `callId`, under its deadline.
    void runEntry(PluginCtx& p, uint64_t callId, const std::function<void()>& body) {
        Call* c = findCall(p, callId);
        if (!c) return;
        writeMarker(p.manifest.id);
        {
            std::lock_guard<std::mutex> lock(runningMu);
            if (stopping) return;
            running = &p.rt;
        }
        const uint64_t outer = p.run->current;
        p.run->current = callId;
        const int64_t began = steadyMs();
        const bool quietRun = c->kind != CallKind::Background && !c->userCaused && !c->quietFree;
        p.rt.setDeadline(c->deadline(began));
        bool timeUp = false, oom = false, poolHit = false;
        auto observe = [&] {
            timeUp = timeUp || p.rt.deadlineHit();
            oom = oom || p.rt.memoryLimitHit();
            poolHit = poolHit || p.rt.poolLimitHit();
        };
        std::string internal;  // a C++ exception from the host's side of the entry
        try {
            body();
            observe();
            if (!timeUp) {
                p.cx.runJobs();
                observe();
            }
        } catch (const std::exception& e) {
            internal = e.what();
        } catch (...) {
            internal = "unknown";
        }
        p.rt.setDeadline(0);
        p.run->current = outer;
        {
            std::lock_guard<std::mutex> lock(runningMu);
            running = nullptr;
        }
        const int64_t ended = steadyMs();
        if (quietRun) noteQuietRun(p, ended, ended - began);
        if (Call* call = findCall(p, callId); call && call->pauses > 0)
            call->ranWhilePausedMs += ended - std::max(began, call->pausedSince);
        auto err = p.cx.takeError();
        if (stopping) return;
        if (!internal.empty()) {
            poisoned(p, callId, internal);
            p.run->unhandled.clear();  // no more JS on this runtime: it is replaced at the next safe point
            return;
        }
        c = findCall(p, callId);
        // Quiet: a cleanup's rejections are the stop already reported; a failed load's module
        // promise rejects with the error just reported.
        const bool quiet = c && (c->kind == CallKind::Cleanup || (c->kind == CallKind::Load && err));
        if (err) {
            if (Call* call = findCall(p, callId)) {
                if (err->limit)
                    stopCall(p, callId, timeUp ? Limit::Time : poolHit ? Limit::Pool : oom ? Limit::Memory : Limit::Stack, true);
                else failCall(p, *call, *err);
            } else {
                log(p.manifest.id, LogLevel::Error, describeError(*err));
            }
        }
        settle(p, quiet);
    }

    // Settles the calls whose function returned and whose promise (if any) is no longer pending,
    // and reports promises rejected with nobody to handle them.
    void settle(PluginCtx& p, bool quiet) {
        std::set<void*> calls;
        for (auto it = p.run->calls.begin(); it != p.run->calls.end();) {
            Call& c = it->second;
            ++it;
            if (!c.returned) continue;
            if (!c.promise.isUndefined()) calls.insert(JS_VALUE_GET_PTR(c.promise.raw()));
            switch (c.promise.promiseState()) {
                case js::PromiseState::Pending: break;
                case js::PromiseState::Rejected: {
                    js::JsError e = describe(p, js::Value(p.cx.raw(), JS_PromiseResult(p.cx.raw(), c.promise.raw())));
                    failCall(p, c, e);
                    break;
                }
                default: succeed(p, c);
            }
        }
        auto unhandled = std::move(p.run->unhandled);
        p.run->unhandled.clear();
        if (quiet || stopping) return;
        for (auto& [key, reason] : unhandled) {
            if (calls.count(key)) continue;
            js::JsError e = describe(p, reason);
            log(p.manifest.id, LogLevel::Warn, "Uncaught (in promise) " + describeError(e));
        }
    }

    // A background task that returned, failed or went over its budget has had its run for the
    // hour; one cut short (by stop(), sleep, the plugin turned off or reloaded) hasn't.
    void backgroundEnded(PluginCtx& p, CallKind kind) {
        if (kind != CallKind::Background || stopping) return;
        uncounted[p.manifest.id] = 0;
        state.setLastBackgroundMs(p.manifest.id, deps.clock());
    }

    // Posts BackgroundDone once the tasks of a runBackground() have all ended.
    void checkBackgroundRound() {
        if (!backgroundOpen || stopping) return;
        for (const auto& [id, p] : loaded)
            for (const auto& [cid, c] : p->run->calls)
                if (c.kind == CallKind::Background) return;
        backgroundOpen = false;
        postEvent({PluginHostEvent::Kind::BackgroundDone, "", "", "", {}, backgroundRound, ""});
    }

    // The call's `done`, at most once.
    void callDone(Call& c, bool ok, const std::string& text) {
        if (!c.done || stopping) return;
        CallDone done = std::move(c.done);
        c.done = nullptr;
        done(c, ok, text);
    }

    void succeed(PluginCtx& p, Call& c) {
        const bool counts = c.resets && (c.kind == CallKind::Load || c.kind == CallKind::Event || c.kind == CallKind::Background);
        if (c.done) {
            JSContext* cx = p.cx.raw();
            std::string text;
            js::Value result = c.promise.isUndefined() ? c.value : js::Value(cx, JS_PromiseResult(cx, c.promise.raw()));
            if (JS_IsString(result.raw())) text = jsString(cx, result.raw());
            callDone(c, true, text);
        }
        backgroundEnded(p, c.kind);
        p.run->calls.erase(c.id);
        if (counts && state.failures(p.manifest.id) > 0) state.clearFailures(p.manifest.id);
    }

    void failCall(PluginCtx& p, Call& c, const js::JsError& e) {
        const std::string text = capitalised(c.what) + " failed: " + describeError(e);
        const bool quiet = c.kind == CallKind::Cleanup;
        if (c.kind == CallKind::Background && e.network && !stopping && uncounted[p.manifest.id] < kMaxUncounted) {
            ++uncounted[p.manifest.id];
            // The connection was lost under the task and the task let that very error escape: not
            // the plugin's fault, so not a failure and not a run for the hour; due again at the next
            // round. Three times in a row at most, then it counts (a plugin can't hide behind it).
            log(p.manifest.id, LogLevel::Warn, text + " (no connection: not counted, tried again next time)");
            p.run->calls.erase(c.id);
            return;
        }
        if (c.kind == CallKind::Load) p.run->loadFailed = true;
        callDone(c, false, e.message.empty() ? e.name : e.message);
        backgroundEnded(p, c.kind);
        p.run->calls.erase(c.id);
        if (quiet) log(p.manifest.id, LogLevel::Error, text);
        else recordFailure(p.manifest.id, text);
    }

    // `interrupted`: the limit cut into running code (not a call that only waited). quickjs-ng
    // leaves an interrupted async function's promise pending forever, so whatever the plugin chained
    // on it would hang: the plugin restarts at the next safe point.
    void stopCall(PluginCtx& p, uint64_t callId, Limit limit, bool interrupted) {
        Call* c = findCall(p, callId);
        if (!c) return;
        const std::string what = c->what;
        const CallKind kind = c->kind;
        const bool onPurpose = limit == Limit::Cancel || limit == Limit::Closed;
        const char* limitName = limit == Limit::Time || onPurpose ? "time" : limit == Limit::Stack ? "stack" : "memory";
        std::string text = "Stopped: " + what;
        if (limit == Limit::Cancel) text += " was cut short (the Kindle went to sleep)";
        else if (limit == Limit::Closed) text += " was stopped: the user closed its pop-up";
        else if (limit == Limit::Time) text += " took longer than " + seconds(c->budgetMs);
        else if (limit == Limit::Memory) text += " used more than " + std::to_string(deps.memoryPerPlugin >> 20) + " MB of memory";
        else if (limit == Limit::Pool) text += " — plugins together used more than " + std::to_string(deps.memoryTotal >> 20) + " MB of memory";
        else text += " ran out of stack (too much recursion)";
        if (kind == CallKind::Load) p.run->loadFailed = true;
        callDone(*c, false, limit == Limit::Time ? "it took longer than " + seconds(c->budgetMs)
                            : limit == Limit::Stack ? "it ran out of stack" : onPurpose ? "" : "it used too much memory");
        p.run->calls.erase(callId);
        if (onPurpose) log(p.manifest.id, LogLevel::Info, text);
        else {
            backgroundEnded(p, kind);
            recordFailure(p.manifest.id, text);  // a stopped cleanup counts too
        }

        auto& run = *p.run;
        for (auto it = run.timers.begin(); it != run.timers.end();) it = it->second.owner == callId ? run.timers.erase(it) : std::next(it);
        withdrawAsks(p, callId);
        bool doomed = false;
        for (auto it = run.work.begin(); it != run.work.end();) {
            if (it->second.owner != callId) {
                ++it;
                continue;
            }
            it->second.cancel->store(true);
            run.doomed.push_back({std::move(it->second), limitName, text});
            it = run.work.erase(it);
            doomed = true;
        }
        if (doomed && net) net->sweep();
        if (stopping) return;
        // Rejected in an entry of its own, posted: never nested in this one, and the loop (other
        // plugins, turning this one off) runs in between.
        if (doomed) inbox->post([serial = run.serial](Impl& h) { h.rejectDoomed(serial); });
        if (interrupted && kind != CallKind::Load) restarts.insert(p.manifest.id);
    }

    // A C++ exception escaped into the entry (natives can't throw, see guardedNative; this is host
    // code around them). The runtime's state is no longer trusted: the call is dropped with its
    // timers, questions and work, counted, and the plugin restarts (a load just fails).
    void poisoned(PluginCtx& p, uint64_t callId, const std::string& internal) {
        Call* c = findCall(p, callId);
        const std::string what = c ? c->what : "the plugin's code";
        const bool load = c && c->kind == CallKind::Load;
        if (load) p.run->loadFailed = true;
        if (c) callDone(*c, false, "the app hit an internal error");
        p.run->calls.erase(callId);
        auto& run = *p.run;
        for (auto it = run.timers.begin(); it != run.timers.end();) it = it->second.owner == callId ? run.timers.erase(it) : std::next(it);
        withdrawAsks(p, callId);
        for (auto it = run.work.begin(); it != run.work.end();) {
            if (it->second.owner != callId) {
                ++it;
                continue;
            }
            it->second.cancel->store(true);
            it = run.work.erase(it);
        }
        if (net) net->sweep();
        recordFailure(p.manifest.id, "Stopped: the app hit an internal error while running " + what + ": " + internal);
        if (!load) restarts.insert(p.manifest.id);
    }

    // Rejects the stopped calls' work with LimitError, so whatever awaited it moves on. The code
    // that runs is a Cleanup call: stopped calls can't start more host work or timers.
    void rejectDoomed(uint64_t serial) {
        PluginCtx* pp = bySerial(serial);
        if (!pp || pp->run->doomed.empty() || restarts.count(pp->manifest.id) || turnOffs.count(pp->manifest.id)) return;
        PluginCtx& p = *pp;
        std::vector<Doomed> doomed = std::move(p.run->doomed);
        p.run->doomed.clear();
        Call cleanup;
        cleanup.id = ++nextCall;
        cleanup.kind = CallKind::Cleanup;
        cleanup.what = doomed.front().work.ownerWhat + " (after it was stopped)";
        cleanup.stopLimit = doomed.front().limit;
        cleanup.stopText = doomed.front().text;
        cleanup.budgetMs = deps.handlerBudgetMs;
        cleanup.startMs = steadyMs();
        cleanup.returned = true;
        const uint64_t cid = cleanup.id;
        p.run->calls.emplace(cid, std::move(cleanup));
        runEntry(p, cid, [&] {
            JSContext* c = p.cx.raw();
            for (auto& d : doomed) {
                JSValue e = newLimitError(p, d.limit, d.text);
                if (JS_IsException(e)) e = JS_GetException(c);
                p.cx.call(d.work.reject, {js::Value(c, e)});
            }
            doomed.clear();
        });
    }

    void recordFailure(const std::string& id, const std::string& text) {
        log(id, LogLevel::Error, text);
        state.recordFailure(id, text);
        if (state.failures(id) >= 3) turnOffs.insert(id);
    }

    std::string describeError(const js::JsError& e) {
        std::string head = e.name.empty() ? e.message : e.name + ": " + e.message;
        std::string stack = e.stack;
        while (!stack.empty() && (stack.back() == '\n' || stack.back() == ' ')) stack.pop_back();
        return stack.empty() ? head : head + "\n" + stack;
    }

    // What a rejected promise's reason says. Reading it may run the plugin's getters, so it runs
    // under a short deadline of its own.
    js::JsError describe(PluginCtx& p, const js::Value& reason) {
        js::JsError e;
        JSContext* c = p.cx.raw();
        p.rt.setDeadline(steadyMs() + 200);
        if (JS_IsObject(reason.raw())) {
            js::Value name = reason.get("name"), message = reason.get("message"), stack = reason.get("stack");
            if (JS_IsString(name.raw())) e.name = jsString(c, name.raw());
            for (const auto& known : p.run->networkErrors)
                if (JS_IsObject(reason.raw()) && JS_VALUE_GET_PTR(known.raw()) == JS_VALUE_GET_PTR(reason.raw())) e.network = true;
            e.message = message.isUndefined() ? jsString(c, reason.raw()) : jsString(c, message.raw());
            if (JS_IsString(stack.raw())) e.stack = jsString(c, stack.raw());
        } else {
            e.message = jsString(c, reason.raw());
        }
        p.rt.setDeadline(0);
        p.cx.takeError();
        return e;
    }

    void checkDeadlines() {
        const int64_t now = steadyMs();
        for (auto& [id, owned] : loaded) {
            PluginCtx& p = *owned;
            std::vector<uint64_t> late;
            for (const auto& [cid, c] : p.run->calls)
                if (!c.pauses && c.deadline(now) <= now) late.push_back(cid);
            for (uint64_t cid : late) {
                if (stopping) return;
                stopCall(p, cid, Limit::Time, false);
            }
        }
    }

    void fireTimers() {
        const int64_t now = steadyMs();
        std::vector<std::string> ids;
        for (const auto& [id, p] : loaded) ids.push_back(id);
        for (const auto& id : ids) {
            auto it = loaded.find(id);
            if (it == loaded.end() || turnOffs.count(id)) continue;
            PluginCtx& p = *it->second;
            std::vector<std::pair<int64_t, uint64_t>> due;
            for (const auto& [tid, t] : p.run->timers)
                if (t.dueMs <= now && !timerPaused(p, t)) due.emplace_back(t.dueMs, tid);
            std::sort(due.begin(), due.end());
            for (const auto& [at, tid] : due) {
                if (stopping || turnOffs.count(id)) break;
                auto t = p.run->timers.find(tid);
                if (t == p.run->timers.end()) continue;  // cleared by an earlier callback
                Timer timer = std::move(t->second);
                p.run->timers.erase(t);
                if (findCall(p, timer.owner)) {
                    runEntry(p, timer.owner, [&] { p.cx.call(timer.callback, {}); });
                } else if (appInBackground) {
                    p.run->timers.emplace(tid, std::move(timer));  // its call ended meanwhile: waits too
                } else if (const int64_t at = quietAllowedAt(p, steadyMs()); at != 0) {
                    timer.dueMs = at;  // waits, under the same id (clearTimeout still works)
                    p.run->timers.emplace(tid, std::move(timer));
                } else {
                    startCall(p, CallKind::Timer, "a timer callback set by " + timer.ownerWhat, deps.handlerBudgetMs, false,
                              [&] { return p.cx.call(timer.callback, {}); });
                }
            }
            // Continuations of ended calls that waited for quiet run time, in order.
            while (!p.run->waiting.empty() && p.run->waiting.front().dueMs <= steadyMs()) {
                if (stopping || turnOffs.count(id)) break;
                if (const int64_t at = quietAllowedAt(p, steadyMs()); at != 0) {
                    p.run->waiting.front().dueMs = at;
                    break;
                }
                auto w = std::move(p.run->waiting.front());
                p.run->waiting.pop_front();
                startCall(p, CallKind::Continued, std::move(w.what), deps.handlerBudgetMs, false, [&] {
                    w.body();
                    return js::Value();
                });
            }
        }
    }

    // ---- Quiet run time -----------------------------------------------------------------------

    void noteQuietRun(PluginCtx& p, int64_t endMs, int64_t ms) {
        auto& runs = p.run->quietRuns;
        runs.emplace_back(endMs, std::max<int64_t>(ms, 0));
        while (!runs.empty() && runs.front().first <= endMs - deps.quietWindowMs) runs.pop_front();
    }

    // 0 if code of an ended call (an orphan timer or continuation) may run now; otherwise when it
    // may: once enough of the window's run time has aged out of it.
    int64_t quietAllowedAt(PluginCtx& p, int64_t now) {
        auto& runs = p.run->quietRuns;
        while (!runs.empty() && runs.front().first <= now - deps.quietWindowMs) runs.pop_front();
        int64_t used = 0;
        for (const auto& r : runs) used += r.second;
        if (used < deps.quietRunMs) {
            p.run->throttled = false;
            return 0;
        }
        int64_t at = now + 1;
        for (const auto& [end, ms] : runs) {
            used -= ms;
            at = end + deps.quietWindowMs;
            if (used < deps.quietRunMs) break;
        }
        if (!p.run->throttled) {
            p.run->throttled = true;
            log(p.manifest.id, LogLevel::Info,
                "Timers wait: outside what the user does, a plugin may run " + seconds(deps.quietRunMs) + " of code every " +
                    seconds(deps.quietWindowMs) + "; this one used it, so its timers run later");
        }
        return std::max(at, now + 1);
    }

    // Code of a call that already ended (a completed piece of its host work): a call of its own,
    // now or when the quiet run time allows.
    void continueEnded(PluginCtx& p, std::string what, std::function<void()> body) {
        if (const int64_t at = quietAllowedAt(p, steadyMs()); at != 0) {
            p.run->waiting.push_back({at, std::move(what), std::move(body)});
            return;
        }
        startCall(p, CallKind::Continued, std::move(what), deps.handlerBudgetMs, false, [&] {
            body();
            return js::Value();
        });
    }

    // ---- Screens -------------------------------------------------------------------------------
    //
    // An open screen runs one thing at a time: its `open` event (when it has onEvent), then
    // render; then each event in turn, each followed by render. Each step is a call (onEvent:
    // user-caused, deps.actionBudgetMs; render: deps.renderBudgetMs, not user-caused); the next
    // step is posted as a task when it ends, so no plugin code runs nested in another's.

    std::string cantShow(PluginCtx& p) { return p.manifest.name + " can't show this screen right now"; }

    void postScreen(PluginCtx& p, PluginHostEvent::Kind kind, uint64_t instance, std::string text) {
        PluginHostEvent ev{kind, p.manifest.id, p.manifest.name, std::move(text), {}, 0, ""};
        ev.instance = instance;
        postEvent(std::move(ev));
    }

    PluginRun::Entry* screenEntry(PluginCtx& p, const std::string& screenId) {
        for (auto& e : p.run->entries)
            if (e.info.kind == "screen" && e.info.id == screenId) return &e;
        return nullptr;
    }

    PluginRun::OpenScreen* screenOf(PluginCtx& p, uint64_t instance) {
        auto it = p.run->screens.find(instance);
        return it == p.run->screens.end() ? nullptr : &it->second;
    }

    PluginCtx* withScreen(uint64_t instance) {
        for (auto& [id, p] : loaded)
            if (p->run->screens.count(instance)) return p.get();
        return nullptr;
    }

    js::Value paramsValue(PluginCtx& p, const std::optional<nlohmann::json>& params) {
        return params ? js::Value::fromJson(p.cx, *params) : js::Value(p.cx.raw(), JS_UNDEFINED);
    }

    void openScreen(uint64_t instance, const std::string& pluginId, const std::string& screenId, std::optional<nlohmann::json> params) {
        activate();
        auto it = loaded.find(pluginId);
        if (it == loaded.end() || turnOffs.count(pluginId) || restarts.count(pluginId) || !screenEntry(*it->second, screenId)) {
            const std::string name = nameOf(pluginId);
            PluginHostEvent ev{PluginHostEvent::Kind::ScreenError, pluginId, name, name + " can't show this screen right now", {}, 0, ""};
            ev.instance = instance;
            postEvent(std::move(ev));
            return;
        }
        startScreen(*it->second, instance, screenId, std::move(params), 0);
        applyTurnOffs();
    }

    void startScreen(PluginCtx& p, uint64_t instance, const std::string& screenId, std::optional<nlohmann::json> params,
                     uint64_t parent) {
        auto& sc = p.run->screens[instance];
        sc.screenId = screenId;
        sc.params = std::move(params);
        sc.parent = parent;
        sc.busy = true;
        if (!screenEntry(p, screenId)->onEvent.isUndefined()) runScreenEvent(p, instance, {{"kind", "open"}});
        else renderScreen(p, instance);
    }

    // zk.ui.open calls of a call that ended well: each screen opens over `parent` (0: none).
    void applyOpens(PluginCtx& p, uint64_t parent, Opens opens) {
        for (auto& [screenId, params] : opens) {
            const uint64_t instance = ++gNextInstance;
            PluginHostEvent ev{PluginHostEvent::Kind::ScreenOpen, p.manifest.id, p.manifest.name,
                               screenEntry(p, screenId)->info.title, {}, 0, ""};
            ev.instance = instance;
            ev.parent = parent;
            ev.screenId = screenId;
            ev.value = params ? *params : nlohmann::json(nullptr);
            postEvent(std::move(ev));
            startScreen(p, instance, screenId, params, parent);
        }
    }

    void runScreenEvent(PluginCtx& p, uint64_t instance, const nlohmann::json& event) {
        PluginRun::OpenScreen& sc = *screenOf(p, instance);
        const js::Value fn = screenEntry(p, sc.screenId)->onEvent;
        const std::optional<nlohmann::json> params = sc.params;
        const uint64_t serial = p.run->serial;
        // `back` comes after the screen is gone: not an answer to the user (no removals asked).
        const bool back = event.value("kind", "") == "back";
        startCall(p, CallKind::Event, "the " + sc.screenId + " screen's onEvent (" + event.value("kind", "") + ")",
                  back ? deps.handlerBudgetMs : deps.actionBudgetMs, !back,
                  [&] { return p.cx.call(fn, {js::Value::fromJson(p.cx, event), paramsValue(p, params)}); }, true,
                  [&](Call& c) {
                      c.screen = instance;
                      c.done = [this, serial, instance](Call& c, bool ok, const std::string& text) {
                          post([serial, instance, ok, text, opens = std::move(c.opens), closes = c.closes,
                                result = std::move(c.closeResult)](Impl& h) mutable {
                              h.screenEventDone(serial, instance, ok, text, std::move(opens), closes, std::move(result));
                          });
                      };
                  });
    }

    void screenEventDone(uint64_t serial, uint64_t instance, bool ok, const std::string& text, Opens opens, bool closes,
                         std::optional<nlohmann::json> result) {
        PluginCtx* pp = bySerial(serial);
        PluginRun::OpenScreen* sc = pp ? screenOf(*pp, instance) : nullptr;
        if (!sc) return;
        PluginCtx& p = *pp;
        if (sc->counting) ++sc->handled;
        sc->counting = false;
        sc->busy = false;
        if (sc->failed) return;
        if (!ok && !text.empty())
            postScreen(p, PluginHostEvent::Kind::ScreenActionError, instance, p.manifest.name + " couldn't do that: " + text);
        if (ok && !sc->closed) {
            applyOpens(p, instance, std::move(opens));
            sc = screenOf(p, instance);
            if (closes) {
                PluginHostEvent ev{PluginHostEvent::Kind::ScreenClose, p.manifest.id, p.manifest.name, "", {}, 0, ""};
                ev.instance = instance;
                ev.value = result ? *result : nlohmann::json(nullptr);
                postEvent(std::move(ev));
                sc->queue.clear();
                sc->closed = true;
                finishScreen(p, instance, std::move(result));
                applyTurnOffs();
                return;
            }
        }
        if (sc->closed) pumpScreen(p, instance);
        else {
            sc->busy = true;
            sc->refreshDue = false;
            renderScreen(p, instance);
        }
        applyTurnOffs();
    }

    // `answer`: the render follows what the user did (an event, the opening), so its time is free;
    // one asked for by zk.ui.refresh counts as quiet run time (a timer can't drive endless renders).
    void renderScreen(PluginCtx& p, uint64_t instance, bool answer = true) {
        PluginRun::OpenScreen& sc = *screenOf(p, instance);
        const js::Value fn = screenEntry(p, sc.screenId)->fn;
        const std::optional<nlohmann::json> params = sc.params;
        const uint64_t serial = p.run->serial;
        startCall(p, CallKind::Render, "the " + sc.screenId + " screen's render", deps.renderBudgetMs, false,
                  [&] { return p.cx.call(p.uiRender, {fn, paramsValue(p, params)}); }, true, [&](Call& c) {
                      c.quietFree = answer;
                      c.done = [this, serial, instance](Call&, bool ok, const std::string& text) {
                          post([serial, instance, ok, text](Impl& h) { h.renderDone(serial, instance, ok, text); });
                      };
                  });
    }

    void renderDone(uint64_t serial, uint64_t instance, bool ok, const std::string& text) {
        PluginCtx* pp = bySerial(serial);
        PluginRun::OpenScreen* sc = pp ? screenOf(*pp, instance) : nullptr;
        if (!sc) return;
        PluginCtx& p = *pp;
        sc->busy = false;
        if (sc->failed) return;
        if (sc->closed) {
            pumpScreen(p, instance);
            return;
        }
        if (!ok) {  // counted as the call's failure
            screenFailed(p, instance);
            return;
        }
        std::string why = "it isn't JSON";
        std::optional<Block> tree;
        if (auto j = nlohmann::json::parse(text, nullptr, false); !j.is_discarded()) tree = parseTree(j, &why);
        if (!tree) {
            recordFailure(p.manifest.id, "The " + sc->screenId + " screen's render returned what can't be shown: " + why);
            screenFailed(p, instance);
            applyTurnOffs();
            return;
        }
        if (state.failures(p.manifest.id) > 0) state.clearFailures(p.manifest.id);
        PluginHostEvent ev{PluginHostEvent::Kind::ScreenTree, p.manifest.id, p.manifest.name, "", {}, sc->handled, ""};
        ev.instance = instance;
        ev.screenId = sc->screenId;
        ev.tree = std::make_shared<const Block>(std::move(*tree));
        postEvent(std::move(ev));
        pumpScreen(p, instance);
        applyTurnOffs();
    }

    void screenFailed(PluginCtx& p, uint64_t instance) {
        PluginRun::OpenScreen& sc = *screenOf(p, instance);
        sc.failed = true;
        sc.queue.clear();
        postScreen(p, PluginHostEvent::Kind::ScreenError, instance, cantShow(p));
    }

    // The screen is gone (closed, and its last events handled): the screen below hears of it.
    void finishScreen(PluginCtx& p, uint64_t instance, std::optional<nlohmann::json> result) {
        PluginRun::OpenScreen* sc = screenOf(p, instance);
        if (!sc) return;
        const uint64_t parent = sc->parent;
        const std::string from = sc->screenId;
        p.run->screens.erase(instance);
        PluginRun::OpenScreen* below = parent ? screenOf(p, parent) : nullptr;
        if (!below || below->closed || below->failed) return;
        nlohmann::json ev = {{"kind", "result"}, {"from", from}};
        if (result) ev["value"] = *result;
        below->queue.emplace_back(std::move(ev), false);
        pumpScreen(p, parent);
    }

    // Starts the screen's next step, if it is idle.
    void pumpScreen(PluginCtx& p, uint64_t instance) {
        PluginRun::OpenScreen* sc = screenOf(p, instance);
        if (!sc || sc->busy || sc->failed || stopping) return;
        if (sc->queue.empty()) {
            if (sc->closed) finishScreen(p, instance, std::nullopt);
            else if (sc->refreshDue) {
                sc->refreshDue = false;
                sc->busy = true;
                renderScreen(p, instance, false);
            }
            return;
        }
        auto [event, counted] = std::move(sc->queue.front());
        sc->queue.pop_front();
        sc->busy = true;
        sc->counting = counted;
        if (screenEntry(p, sc->screenId)->onEvent.isUndefined()) {
            post([serial = p.run->serial, instance](Impl& h) { h.screenEventDone(serial, instance, true, "", {}, false, {}); });
            return;
        }
        runScreenEvent(p, instance, event);
    }

    void screenEvent(uint64_t instance, nlohmann::json event) {
        PluginCtx* p = withScreen(instance);
        PluginRun::OpenScreen* sc = p ? screenOf(*p, instance) : nullptr;
        if (!sc || sc->closed || sc->failed) return;
        ++sc->received;
        sc->queue.emplace_back(std::move(event), true);
        pumpScreen(*p, instance);
        applyTurnOffs();
    }

    void closeScreen(uint64_t instance) {
        PluginCtx* p = withScreen(instance);
        PluginRun::OpenScreen* sc = p ? screenOf(*p, instance) : nullptr;
        if (!sc || sc->closed) return;
        sc->closed = true;
        if (sc->failed) {
            sc->failed = false;  // nothing more runs for it; it goes now
            sc->queue.clear();
        } else {
            sc->queue.emplace_back(nlohmann::json{{"kind", "back"}}, false);
        }
        pumpScreen(*p, instance);
        applyTurnOffs();
    }

    // zk.ui.refresh: the plugin's showing screen (the last opened that is still open) renders again.
    void refreshScreen(uint64_t serial) {
        PluginCtx* p = bySerial(serial);
        if (!p) return;
        for (auto it = p->run->screens.rbegin(); it != p->run->screens.rend(); ++it) {
            if (it->second.closed || it->second.failed) continue;
            it->second.refreshDue = true;
            pumpScreen(*p, it->first);
            break;
        }
        applyTurnOffs();
    }

    // ---- Actions -------------------------------------------------------------------------------
    //
    // An action's run is a user-caused call with the action budget; the runner (installEntry)
    // hands back "" or the JSON text of what it returned, checked here when the call has ended.

    void postAction(PluginHostEvent::Kind kind, const std::string& pluginId, const std::string& name, uint64_t run,
                    std::string text, nlohmann::json value = nullptr) {
        PluginHostEvent ev{kind, pluginId, name, std::move(text), {}, run, ""};
        ev.value = std::move(value);
        postEvent(std::move(ev));
    }

    void runAction(uint64_t run, const std::string& pluginId, const std::string& kind, const std::string& actionId,
                   const nlohmann::ordered_json& target, const nlohmann::json& again) {
        activate();
        auto it = loaded.find(pluginId);
        const PluginRun::Entry* entry = nullptr;
        if (it != loaded.end() && !turnOffs.count(pluginId) && !restarts.count(pluginId))
            for (const auto& e : it->second->run->entries)
                if (e.info.kind == kind && e.info.id == actionId) entry = &e;
        if (!entry) {
            const std::string name = nameOf(pluginId);
            postAction(PluginHostEvent::Kind::ActionError, pluginId, name, run, name + " couldn't do that: it isn't running");
            return;
        }
        PluginCtx& p = *it->second;
        const uint64_t serial = p.run->serial;
        {  // no Value of the plugin may outlive it: turning it off frees its runtime
            const js::Value fn = entry->fn;
            startCall(p, CallKind::Event, "the " + actionId + " action", deps.actionBudgetMs, true,
                      [&] {
                          return p.cx.call(p.actionRunner, {fn, js::Value::fromOrderedJson(p.cx, target),
                                                            again.is_null() ? js::Value(p.cx.raw(), JS_UNDEFINED) : js::Value::fromJson(p.cx, again)});
                      },
                      true, [&](Call& c) {
                          c.action = true;
                          c.actionRun = run;
                          c.done = [this, serial, run, pluginId, name = p.manifest.name](Call& c, bool ok, const std::string& text) {
                              post([serial, run, ok, text, pluginId, name, opens = std::move(c.opens)](Impl& h) mutable {
                                  h.actionDone(serial, run, pluginId, name, ok, text, std::move(opens));
                              });
                          };
                      });
        }
        applyTurnOffs();
    }

    static constexpr size_t kMaxReplyBytes = 64 * 1024;

    // What run returned, as the pop-up takes it: {text, actions} or a discarded value (*why says why).
    static nlohmann::json replyOf(const std::string& text, std::string* why) {
        auto j = nlohmann::json::parse(text, nullptr, false);
        const nlohmann::json bad = nlohmann::json::value_t::discarded;
        if (!j.is_object() || !j.contains("text") || !j["text"].is_string()) {
            *why = "expected a string or { text, actions }";
            return bad;
        }
        std::string t = j["text"].get<std::string>();
        if (t.size() > kMaxReplyBytes) {
            size_t cut = kMaxReplyBytes;
            while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
            t = t.substr(0, cut) + "…";
        }
        nlohmann::json out = {{"text", t}, {"actions", nlohmann::json::array()}};
        if (j.contains("actions") && !j["actions"].is_null()) {
            const auto& a = j["actions"];
            if (!a.is_array() || a.size() > 2) {
                *why = "`actions` must be a list of at most 2 buttons";
                return bad;
            }
            for (const auto& b : a) {
                if (!b.is_object() || !b.contains("id") || !b["id"].is_string() || b["id"].get<std::string>().empty() ||
                    !b.contains("title") || !b["title"].is_string() || oneLine(b["title"].get<std::string>()).empty() ||
                    oneLine(b["title"].get<std::string>()).size() > 120) {
                    *why = "each of `actions` must be { id, title } with a short title";
                    return bad;
                }
                out["actions"].push_back({{"id", b["id"]}, {"title", oneLine(b["title"].get<std::string>())}});
            }
        }
        return out;
    }

    void actionDone(uint64_t serial, uint64_t run, const std::string& pluginId, const std::string& name, bool ok,
                    const std::string& text, Opens opens) {
        if (stopping) return;
        if (!ok) {  // stopped by the user (or the app): nobody waits; else the error
            if (!text.empty()) postAction(PluginHostEvent::Kind::ActionError, pluginId, name, run, name + " couldn't do that: " + text);
            return;
        }
        nlohmann::json value = nullptr;
        if (!text.empty()) {
            std::string why;
            value = replyOf(text, &why);
            if (value.is_discarded()) {
                log(pluginId, LogLevel::Error, "The action's run returned what can't be shown: " + why);
                postAction(PluginHostEvent::Kind::ActionError, pluginId, name, run,
                           name + " couldn't do that: it returned what can't be shown (" + why + ")");
                return;
            }
        }
        postAction(PluginHostEvent::Kind::ActionResult, pluginId, name, run, "", std::move(value));
        if (PluginCtx* p = bySerial(serial)) {
            applyOpens(*p, 0, std::move(opens));
            applyTurnOffs();
        }
    }

    void stopAction(uint64_t run) {
        for (auto& [id, owned] : loaded) {
            PluginCtx& p = *owned;
            for (const auto& [cid, c] : p.run->calls)
                if (c.actionRun == run) {
                    stopCall(p, cid, Limit::Closed, false);
                    return;
                }
        }
    }

    // ---- Commands ----------------------------------------------------------------------------

    void dispatch(const PluginEvent& ev) {
        activate();
        const nlohmann::ordered_json payload = ev.payload.is_null() ? nlohmann::ordered_json::object() : ev.payload;
        std::vector<std::string> ids;
        for (const auto& [id, p] : loaded) ids.push_back(id);
        for (const auto& id : ids) {
            auto it = loaded.find(id);
            if (it == loaded.end()) continue;
            PluginCtx& p = *it->second;
            auto h = p.run->handlers.find(ev.name);
            if (h == p.run->handlers.end()) continue;
            {
                const std::vector<js::Value> handlers = h->second;  // a handler may register more
                for (const auto& handler : handlers) {
                    if (stopping || turnOffs.count(id)) break;
                    startCall(p, CallKind::Event, "the " + ev.name + " handler", deps.handlerBudgetMs, ev.userCaused,
                              [&] { return p.cx.call(handler, {js::Value::fromOrderedJson(p.cx, payload)}); });
                }
            }  // no Value of the plugin may outlive it: turning it off frees its runtime
            applyTurnOffs();
        }
    }

    static constexpr int kMaxUncounted = 3;
    static constexpr int64_t kBackgroundEveryMs = 60 * 60 * 1000;

    // Runs each loaded plugin's background task that is due: not run, failed or stopped for time
    // in the last hour (a task cut short counts as not run). Exactly one BackgroundDone follows
    // once they have all ended (at once if none was due).
    void runBackground(uint64_t round) {
        activate();
        backgroundOpen = true;
        backgroundRound = round;
        std::vector<std::string> ids;
        for (const auto& [id, p] : loaded) ids.push_back(id);
        const int64_t now = deps.clock();
        for (const auto& id : ids) {
            auto it = loaded.find(id);
            if (it == loaded.end() || stopping || turnOffs.count(id)) continue;
            PluginCtx& p = *it->second;
            if (p.run->background.isUndefined()) continue;
            const int64_t last = state.lastBackgroundMs(id);
            if (last != 0 && last <= now && now - last < kBackgroundEveryMs) continue;
            bool running = false;
            for (const auto& [cid, c] : p.run->calls) running = running || c.kind == CallKind::Background;
            if (running) continue;
            {
                const js::Value task = p.run->background;
                startCall(p, CallKind::Background, "the background task", deps.backgroundBudgetMs, false,
                          [&] { return p.cx.call(task, {}); });
            }
            applyTurnOffs();
        }
        checkBackgroundRound();
    }

    // The Kindle is going to sleep: tasks still running are cut short, uncounted.
    void cancelBackground() {
        std::vector<std::string> ids;
        for (const auto& [id, p] : loaded) ids.push_back(id);
        for (const auto& id : ids) {
            auto it = loaded.find(id);
            if (it == loaded.end() || stopping) continue;
            PluginCtx& p = *it->second;
            std::vector<uint64_t> calls;
            for (const auto& [cid, c] : p.run->calls)
                if (c.kind == CallKind::Background) calls.push_back(cid);
            for (uint64_t cid : calls) stopCall(p, cid, Limit::Cancel, false);
        }
        checkBackgroundRound();
    }

    void answer(uint64_t requestId, bool yes) {
        for (auto& [id, owned] : loaded) {
            PluginCtx& p = *owned;
            auto it = p.run->asks.find(requestId);
            if (it == p.run->asks.end()) continue;
            Ask ask = std::move(it->second);
            p.run->asks.erase(it);
            if (Call* c = findCall(p, ask.owner); c && c->pauses > 0 && --c->pauses == 0) c->pausedMs += steadyMs() - c->pausedSince;
            ask.answered(yes);
            return;
        }
    }

    void reload(const std::string* only) {
        std::vector<std::string> reloadNow;
        for (const auto& f : foldersCopy()) {
            if (only && f.dir != *only) continue;
            if (loaded.count(f.dir) || deps.loadEagerly) reloadNow.push_back(f.dir);
            unload(f.dir);
            failedLoad.erase(f.dir);
            setProblem(f.dir, "");
        }
        if (only) failedLoad.erase(*only);
        if (only && deps.loadEagerly && !std::count(reloadNow.begin(), reloadNow.end(), *only)) reloadNow.push_back(*only);  // maybe a new folder
        auto fresh = scanPlugins(deps.pluginsDir, deps.appVersion);
        {
            std::lock_guard<std::mutex> lock(mu);
            folders = std::move(fresh);
        }
        for (const auto& f : foldersCopy())
            if (std::count(reloadNow.begin(), reloadNow.end(), f.dir) && f.problem.empty() && f.manifest && state.enabled(f.dir)) {
                load(f);
                applyTurnOffs();
            }
    }

    void post(Task t) { inbox->post(std::move(t)); }
};

// ---- PluginCtx and the binding helpers ---------------------------------------------------------

namespace plugins {

PluginCtx::PluginCtx(PluginHost::Impl& h, PluginManifest m, std::string d, size_t memoryLimit)
    : host(h), manifest(std::move(m)), dir(std::move(d)), dataDir(dir + "/data"), rt(memoryLimit, &h.pool), cx(rt),
      run(std::make_unique<PluginRun>()) {}

PluginCtx::~PluginCtx() {
    run.reset();  // its Values before the context
}

PluginCtx& ctxOf(JSContext* c) { return *static_cast<PluginCtx*>(JS_GetContextOpaque(c)); }

PluginNet* netOf(PluginCtx& p) {
    auto& h = p.host;
    if (!h.net && h.deps.makeHttp)
        h.net = std::make_unique<PluginNet>(h.deps.makeHttp, [&h] {
            std::lock_guard<std::mutex> lock(h.mu);
            return h.webdavHost;
        });
    return h.net.get();
}

const PluginHostDeps& depsOf(PluginCtx& p) { return p.host.deps; }

PluginState& pluginStateOf(PluginCtx& p) { return p.host.state; }

std::shared_ptr<void>& librarySlot(PluginCtx& p) { return p.host.library; }

CallInfo currentCall(PluginCtx& p) {
    auto it = p.run->calls.find(p.run->current);
    if (it == p.run->calls.end()) return {};
    return {it->first, it->second.userCaused, it->second.kind == CallKind::Background, it->second.kind == CallKind::Cleanup};
}

Work::Work(std::weak_ptr<Inbox> inbox, uint64_t ctxSerial, uint64_t id) : inbox_(std::move(inbox)), ctxSerial_(ctxSerial), id_(id) {}

void Work::finish(Settle settle) {
    if (finished_.exchange(true) || cancelled()) return;
    auto inbox = inbox_.lock();
    if (!inbox) return;
    inbox->post([serial = ctxSerial_, id = id_, settle = std::move(settle)](PluginHost::Impl& h) mutable {
        PluginCtx* p = h.bySerial(serial);
        if (!p) return;
        auto it = p->run->work.find(id);
        if (it == p->run->work.end()) return;  // cancelled meanwhile
        PendingWork w = std::move(it->second);
        p->run->work.erase(it);
        const uint64_t owner = w.owner;
        std::string what = w.ownerWhat + ", continuing after it ended";
        auto body = [p, w = std::move(w), settle = std::move(settle)] {
            JSContext* c = p->cx.raw();
            JSValue v;
            try {
                v = settle(c);
            } catch (const std::exception& e) {  // the host's work failed: the plugin sees an Error
                v = JS_ThrowPlainError(c, "The app failed doing this: %s", e.what());
            }
            if (JS_IsException(v)) p->cx.call(w.reject, {js::Value(c, JS_GetException(c))});
            else p->cx.call(w.resolve, {js::Value(c, v)});
        };
        if (h.findCall(*p, owner)) h.runEntry(*p, owner, body);
        else h.continueEnded(*p, std::move(what), std::move(body));
    });
}

void Work::resolve(nlohmann::json value) {
    finish([value = std::move(value)](JSContext* c) {
        const std::string text = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return JS_ParseJSON(c, text.data(), text.size(), "<json>");
    });
}

void Work::reject(std::string message) {
    finish([message = std::move(message)](JSContext* c) {
        JSValue e = JS_NewError(c);
        JS_SetPropertyStr(c, e, "message", JS_NewStringLen(c, message.data(), message.size()));
        JS_Throw(c, e);
        return JS_EXCEPTION;
    });
}

std::shared_ptr<Work> startWork(PluginCtx& p, JSValue* promise) {
    JSContext* c = p.cx.raw();
    JSValue funcs[2];
    *promise = JS_NewPromiseCapability(c, funcs);
    auto& h = p.host;
    auto work = std::make_shared<Work>(h.inbox, p.run->serial, ++h.nextWork);
    if (JS_IsException(*promise)) {
        work->cancelFlag()->store(true);
        return work;
    }
    auto it = p.run->calls.find(p.run->current);
    if (it != p.run->calls.end() && it->second.kind == CallKind::Cleanup) {
        // Code of a stopped call: no new host work; reject at once with the stop's LimitError.
        work->cancelFlag()->store(true);
        JSValue e = newLimitError(p, it->second.stopLimit, it->second.stopText);
        if (JS_IsException(e)) e = JS_GetException(c);
        JSValue r = JS_Call(c, funcs[1], JS_UNDEFINED, 1, &e);
        JS_FreeValue(c, r);
        JS_FreeValue(c, e);
        JS_FreeValue(c, funcs[0]);
        JS_FreeValue(c, funcs[1]);
        return work;
    }
    PendingWork w;
    w.owner = p.run->current;
    w.ownerWhat = it == p.run->calls.end() ? "the plugin" : it->second.what;
    w.resolve = js::Value(c, funcs[0]);
    w.reject = js::Value(c, funcs[1]);
    w.cancel = work->cancelFlag();
    p.run->work.emplace(work->id(), std::move(w));
    return work;
}

uint64_t askUser(PluginCtx& p, PluginHostEvent confirm, std::function<void(bool)> answered) {
    auto& h = p.host;
    if (Call* c = h.findCall(p, p.run->current); c && c->kind == CallKind::Cleanup) return 0;  // a stopped call can't ask
    if (Call* c = h.findCall(p, p.run->current); c && c->questions >= kMaxQuestions) {
        h.log(p.manifest.id, LogLevel::Warn,
              "Not asked: " + c->what + " already asked " + std::to_string(kMaxQuestions) +
                  " questions, the most one action may ask (resolved null)");
        return 0;
    } else if (c) {
        ++c->questions;
    }
    const uint64_t id = ++h.nextRequest;
    confirm.kind = PluginHostEvent::Kind::Confirm;
    confirm.requestId = id;
    const uint64_t owner = p.run->current;
    if (Call* c = h.findCall(p, owner); c && c->pauses++ == 0) c->pausedSince = steadyMs();
    p.run->asks.emplace(id, Ask{owner, std::move(answered)});
    post(p, std::move(confirm));
    return id;
}

void post(PluginCtx& p, PluginHostEvent ev) {
    ev.pluginId = p.manifest.id;
    ev.pluginName = p.manifest.name;
    p.host.postEvent(std::move(ev));
}

void log(PluginCtx& p, LogLevel level, const std::string& text) { p.host.log(p.manifest.id, level, text); }

uint64_t addTimer(PluginCtx& p, js::Value callback, int64_t delayMs) {
    auto& h = p.host;
    if (p.run->timers.size() >= kMaxTimers) return 0;
    auto owner = p.run->calls.find(p.run->current);
    const bool cleanup = owner != p.run->calls.end() && owner->second.kind == CallKind::Cleanup;
    Timer t;
    t.dueMs = steadyMs() + std::max<int64_t>(delayMs, h.deps.minTimerMs);
    // A stopped call's cleanup code owns nothing: its timer runs as a call of its own.
    t.owner = cleanup ? 0 : p.run->current;
    t.ownerWhat = owner == p.run->calls.end() ? "the plugin" : owner->second.what;
    t.callback = std::move(callback);
    const uint64_t id = ++h.nextTimer;
    p.run->timers.emplace(id, std::move(t));
    return id;
}

void clearTimer(PluginCtx& p, uint64_t id) { p.run->timers.erase(id); }

bool addHandler(PluginCtx& p, const std::string& event, js::Value handler) {
    auto& list = p.run->handlers[event];
    if (list.size() >= kMaxHandlersPerEvent) return false;
    list.push_back(std::move(handler));
    return true;
}

void noteNetworkError(PluginCtx& p, js::Value error) {
    auto& list = p.run->networkErrors;
    list.push_back(std::move(error));
    if (list.size() > 8) list.erase(list.begin());
}

bool hasEntry(PluginCtx& p, const std::string& kind, const std::string& id) {
    for (const auto& e : p.run->entries)
        if (e.info.kind == kind && e.info.id == id) return true;
    return false;
}

void addEntry(PluginCtx& p, const std::string& kind, const std::string& id, const std::string& title, js::Value fn) {
    PluginEntry info{kind, id, title};
    p.run->entries.push_back({info, std::move(fn), {}});
    ++p.host.version;
    {
        std::lock_guard<std::mutex> lock(p.host.mu);
        p.host.entryList[p.manifest.id].push_back(std::move(info));
    }
    wakeUi();  // screens showing plugins' entries look at the version
}

void addScreen(PluginCtx& p, const std::string& id, const std::string& title, bool main, js::Value render, js::Value onEvent) {
    PluginEntry info{"screen", id, title, main};
    p.run->entries.push_back({info, std::move(render), std::move(onEvent)});
    ++p.host.version;
    {
        std::lock_guard<std::mutex> lock(p.host.mu);
        p.host.entryList[p.manifest.id].push_back(std::move(info));
    }
    wakeUi();  // screens showing plugins' entries look at the version
}

std::string uiOpen(PluginCtx& p, const std::string& screenId, std::optional<nlohmann::json> params) {
    Call* c = p.host.findCall(p, p.run->current);
    if (!c || c->kind == CallKind::Cleanup || (c->screen == 0 && !c->action))
        return "zk.ui.open: only in answer to the user (from a screen's onEvent or an action's run)";
    if (!p.host.screenEntry(p, screenId)) return "zk.ui.open: there is no screen \"" + screenId + "\" (register it with zk.screen)";
    if (c->opens.size() >= 3) return "zk.ui.open: at most 3 screens at once";
    c->opens.emplace_back(screenId, std::move(params));
    return "";
}

std::string uiClose(PluginCtx& p, std::optional<nlohmann::json> result) {
    Call* c = p.host.findCall(p, p.run->current);
    if (!c || c->kind == CallKind::Cleanup || c->screen == 0) return "zk.ui.close: only from a screen's onEvent";
    c->closes = true;
    c->closeResult = std::move(result);
    return "";
}

void uiRefresh(PluginCtx& p) {
    if (Call* c = p.host.findCall(p, p.run->current); c && c->kind == CallKind::Render) return;  // render asking for render
    p.host.post([serial = p.run->serial](PluginHost::Impl& h) { h.refreshScreen(serial); });
}

void setBackgroundTask(PluginCtx& p, js::Value task) {
    p.run->background = std::move(task);
    std::lock_guard<std::mutex> lock(p.host.mu);
    p.host.withTask.insert(p.manifest.id);
}

}  // namespace plugins

void plugins::lowerThreadPriority() {
#if defined(__APPLE__)
    // macOS (the desktop build): a lower scheduling priority within the default QoS class. A lower
    // QoS class (utility) would also coalesce its timers, which makes plugin timers late.
    int policy = 0;
    sched_param param{};
    if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
        param.sched_priority = std::max(sched_get_priority_min(policy), param.sched_priority - kPluginNice);
        pthread_setschedparam(pthread_self(), policy, &param);
    }
#else
    // Linux: nice is per thread (the tid), raised from what it inherited; no privilege needed.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), kPluginNice);
#endif
}

// ---- PluginHost ---------------------------------------------------------------------------------

PluginHost::PluginHost(PluginHostDeps deps) : impl_(std::make_unique<Impl>(std::move(deps))) {}

PluginHost::~PluginHost() { stop(); }

namespace {
void* threadMain(void* impl) {
    plugins::lowerThreadPriority();
    static_cast<PluginHost::Impl*>(impl)->loop();
    return nullptr;
}
}  // namespace

void PluginHost::start() {
    if (impl_->started) return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // The engine's stack limit is 512 KB; secondary threads may default to less than that.
    pthread_attr_setstacksize(&attr, 4u << 20);
    impl_->started = pthread_create(&impl_->thread, &attr, threadMain, impl_.get()) == 0;
    pthread_attr_destroy(&attr);
    if (impl_->started && impl_->deps.loadEagerly) impl_->post([](Impl& h) { h.reload(nullptr); });
}

void PluginHost::stop() {
    Impl& h = *impl_;
    if (!h.started || h.joined) return;
    {
        std::lock_guard<std::mutex> lock(h.inbox->mu);
        h.inbox->stopping = true;
    }
    h.stopping = true;
    h.inbox->wake.notify_all();
    {
        std::lock_guard<std::mutex> lock(h.runningMu);
        if (h.running) h.running->interruptNow();
    }
    pthread_join(h.thread, nullptr);
    h.joined = true;
    if (h.net) h.net->shutdown();  // aborts fetches still running; their promises are gone with the thread
}

void PluginHost::reload() {
    impl_->post([](Impl& h) { h.reload(nullptr); });
}

void PluginHost::reload(const std::string& id) {
    impl_->post([id](Impl& h) { h.reload(&id); });
}

void PluginHost::resetData(const std::string& id) {
    impl_->post([id](Impl& h) { h.resetData(id); });
}

void PluginHost::dispatch(PluginEvent ev) {
    impl_->post([ev = std::move(ev)](Impl& h) { h.dispatch(ev); });
}

void PluginHost::runBackground(uint64_t round) {
    impl_->post([round](Impl& h) { h.runBackground(round); });
}

bool PluginHost::anyEnabled() const {
    Impl& h = *impl_;
    std::lock_guard<std::mutex> lock(h.mu);
    for (const auto& f : h.folders)
        if (f.manifest && f.problem.empty() && h.state.enabled(f.dir)) return true;
    return false;
}

bool PluginHost::backgroundDue() const {
    Impl& h = *impl_;
    const int64_t now = h.deps.clock();
    std::lock_guard<std::mutex> lock(h.mu);
    for (const auto& f : h.folders) {
        if (!f.manifest || !f.problem.empty() || !f.manifest->permissions.count(Permission::Background)) continue;
        if (!h.state.enabled(f.dir)) continue;
        if (h.loadedIds.count(f.dir) && !h.withTask.count(f.dir)) continue;  // loaded, and registered none
        const int64_t last = h.state.lastBackgroundMs(f.dir);
        if (last == 0 || last > now || now - last >= Impl::kBackgroundEveryMs) return true;
    }
    return false;
}

void PluginHost::cancelBackground() {
    impl_->post([](Impl& h) { h.cancelBackground(); });
}

void PluginHost::setBackground(bool background) {
    impl_->post([background](Impl& h) { h.appInBackground = background; });
}

uint64_t PluginHost::openScreen(const std::string& pluginId, const std::string& screenId, nlohmann::json params) {
    const uint64_t instance = ++gNextInstance;
    std::optional<nlohmann::json> p;
    if (!params.is_null()) p = std::move(params);
    impl_->post([instance, pluginId, screenId, p = std::move(p)](Impl& h) mutable { h.openScreen(instance, pluginId, screenId, std::move(p)); });
    return instance;
}

void PluginHost::screenEvent(uint64_t instance, nlohmann::json event) {
    impl_->post([instance, event = std::move(event)](Impl& h) mutable { h.screenEvent(instance, std::move(event)); });
}

void PluginHost::closeScreen(uint64_t instance) {
    impl_->post([instance](Impl& h) { h.closeScreen(instance); });
}

uint64_t PluginHost::runAction(const std::string& pluginId, const std::string& kind, const std::string& actionId,
                               nlohmann::ordered_json target, nlohmann::json again) {
    const uint64_t run = ++gNextRun;
    impl_->post([run, pluginId, kind, actionId, target = std::move(target), again = std::move(again)](Impl& h) {
        h.runAction(run, pluginId, kind, actionId, target, again);
    });
    return run;
}

void PluginHost::stopAction(uint64_t run) {
    impl_->post([run](Impl& h) { h.stopAction(run); });
}

void PluginHost::wake() {
    impl_->post([](Impl& h) { h.activate(); });
}

uint64_t PluginHost::entriesVersion() const { return impl_->version.load(); }

void PluginHost::answer(uint64_t requestId, bool yes) {
    impl_->post([requestId, yes](Impl& h) { h.answer(requestId, yes); });
}

bool PluginHost::poll(PluginHostEvent& out) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->events.empty()) return false;
    out = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

std::vector<PluginStatus> PluginHost::status() const {
    Impl& h = *impl_;
    std::vector<PluginStatus> out;
    std::lock_guard<std::mutex> lock(h.mu);
    for (const auto& f : h.folders) {
        PluginStatus s;
        s.folder = f;
        s.enabled = h.state.enabled(f.dir);
        s.everEnabled = h.state.everEnabled(f.dir);
        s.stopped = h.state.stopped(f.dir);
        s.failures = h.state.failures(f.dir);
        s.loaded = h.loadedIds.count(f.dir) > 0;
        s.badge = h.state.badge(f.dir);
        if (auto w = h.withdrawn.find(f.dir); w != h.withdrawn.end()) s.withdrawn = w->second;
        if (auto e = h.entryList.find(f.dir); e != h.entryList.end() && s.loaded) s.entries = e->second;
        s.errors = h.state.lastErrors(f.dir);
        if (auto p = h.problems.find(f.dir); p != h.problems.end()) {
            s.problem = p->second;
            s.errors.push_back(p->second);
        }
        out.push_back(std::move(s));
    }
    return out;
}

void PluginHost::setEnabled(const std::string& id, bool on) {
    {  // only a folder it lists; turned on, only one that can load
        std::lock_guard<std::mutex> lock(impl_->mu);
        const auto& fs_ = impl_->folders;
        auto f = std::find_if(fs_.begin(), fs_.end(), [&](const PluginFolder& f) { return f.dir == id; });
        if (f == fs_.end() || (on && (!f->manifest || !f->problem.empty() || impl_->withdrawn.count(id)))) return;
    }
    impl_->state.setEnabled(id, on);
    ++impl_->version;
    if (on) impl_->state.clearFailures(id);
    // Developer mode (loadEagerly): a plugin turned on loads at once, as at start. Otherwise
    // (the Kindle) it waits for its first event. Before start(), start() loads it.
    const bool loadNow = on && impl_->started && impl_->deps.loadEagerly;
    impl_->post([id, on, loadNow](Impl& h) {
        h.failedLoad.erase(id);
        h.setProblem(id, "");
        if (!on) h.unload(id);
        else if (loadNow && !h.loaded.count(id)) h.reload(&id);
    });
}

void PluginHost::remove(const std::string& id) { forgetPlugin(id, true); }

void PluginHost::forget(const std::string& id) { forgetPlugin(id, false); }

void PluginHost::forgetState(const std::string& id) {
    impl_->state.forget(id);
    ++impl_->version;
}

void PluginHost::forgetPlugin(const std::string& id, bool deleteFolder) {
    Impl& h = *impl_;
    {
        std::lock_guard<std::mutex> lock(h.mu);
        auto& fs_ = h.folders;
        const auto before = fs_.size();
        fs_.erase(std::remove_if(fs_.begin(), fs_.end(), [&](const PluginFolder& f) { return f.dir == id; }), fs_.end());
        if (fs_.size() == before) return;  // not one of ours (and never a path)
        h.problems.erase(id);
    }
    h.state.forget(id);
    ++h.version;
    h.post([id, deleteFolder](Impl& h) {
        h.failedLoad.erase(id);
        h.turnOffs.erase(id);
        h.restarts.erase(id);
        h.unload(id);
        h.logs.erase(id);
        std::error_code ec;
        if (deleteFolder) fs::remove_all(fs::path(h.deps.pluginsDir) / id, ec);  // what stays (it failed) is listed again by a rescan
        h.state.forget(id);  // what a call ending meanwhile recorded
    });
}

bool PluginHost::hold(const std::string& id, int timeoutMs) {
    Impl& h = *impl_;
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.held.insert(id);
    }
    ++h.version;
    if (!h.started || h.joined) return true;  // no plugin thread: nothing runs
    auto done = std::make_shared<std::promise<void>>();
    auto stopped = done->get_future();
    h.post([id, done](Impl& h) {
        h.unload(id);
        h.logs.erase(id);  // reopened in its new folder
        done->set_value();
    });
    // Until it is unloaded, the host stops (nothing runs after that), or the time is up.
    for (int waited = 0; waited < timeoutMs; waited += 50)
        if (stopped.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready || h.stopping) return true;
    return false;
}

void PluginHost::release(const std::string& id) {
    Impl& h = *impl_;
    {
        std::lock_guard<std::mutex> lock(h.mu);
        h.held.erase(id);
    }
    ++h.version;
    if (h.started && !h.joined) {
        h.post([id](Impl& h) { h.reload(&id); });
        return;
    }
    auto fresh = scanPlugins(h.deps.pluginsDir, h.deps.appVersion);  // no plugin thread to do it
    std::lock_guard<std::mutex> lock(h.mu);
    h.folders = std::move(fresh);
}

void PluginHost::setWithdrawn(std::map<std::string, std::string> withdrawn) {
    Impl& h = *impl_;
    std::vector<std::string> off;
    {
        std::lock_guard<std::mutex> lock(h.mu);
        if (h.withdrawn == withdrawn) return;
        for (const auto& [id, why] : withdrawn)
            if (!h.withdrawn.count(id)) off.push_back(id);
        h.withdrawn = std::move(withdrawn);
    }
    for (const auto& id : off) {
        h.state.turnOff(id);
        h.post([id](Impl& h) {
            h.unload(id);
            h.log(id, LogLevel::Warn, "Withdrawn from the catalog — turned off");
        });
    }
    ++h.version;
}

void PluginHost::setWebdavHost(std::string host) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->webdavHost = std::move(host);
}

bool PluginHost::waitIdle(int timeoutMs) {
    Inbox& in = *impl_->inbox;
    std::unique_lock<std::mutex> lock(in.mu);
    return in.idle.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return in.tasks.empty() && !in.busy; });
}

}  // namespace zk

