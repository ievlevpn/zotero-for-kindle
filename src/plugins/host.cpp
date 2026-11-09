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
// (the event budget). Waiting for the user (askUser) pauses the call's clock.
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
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>

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

enum class CallKind { Load, Event, Background, Timer, Continued, Cleanup };

struct Call {
    uint64_t id = 0;
    CallKind kind = CallKind::Event;
    std::string what;  // "the syncFinished handler"
    int budgetMs = 0;
    int64_t startMs = 0, pausedMs = 0, pausedSince = 0;
    int pauses = 0;
    bool userCaused = false;
    bool resets = true;     // success clears the failure count (not a restart's reload)
    std::string stopLimit, stopText;  // a cleanup: why its call was stopped
    bool returned = false;  // its function has returned; `promise` holds what it returned, if a promise
    js::Value promise;
    int64_t deadline(int64_t now) const { return startMs + budgetMs + pausedMs + (pauses ? now - pausedSince : 0); }
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
    std::map<uint64_t, Timer> timers;
    std::map<uint64_t, PendingWork> work;
    std::vector<Doomed> doomed;
    std::map<uint64_t, Ask> asks;  // by request id
    std::vector<std::pair<void*, js::Value>> unhandled;  // rejected promises nobody handled yet
    bool loadFailed = false;
};

}  // namespace plugins

using namespace plugins;

namespace {

int64_t steadyMs() { return js::Runtime::nowMs(); }

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
    enum class Limit { Time, Memory, Pool, Stack };

    PluginHostDeps deps;
    PluginState state;
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    pthread_t thread{};
    bool started = false, joined = false;
    std::atomic<bool> stopping{false};

    // Shared with the screen thread.
    mutable std::mutex mu;  // folders, loadedIds, problems, events
    std::vector<PluginFolder> folders;
    std::set<std::string> loadedIds;
    std::map<std::string, std::string> problems;  // a current problem, shown on its row
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
        std::lock_guard<std::mutex> lock(mu);
        events.push_back(std::move(ev));
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
        std::lock_guard<std::mutex> lock(mu);
        if (on) loadedIds.insert(id);
        else loadedIds.erase(id);
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
        state.setEnabled(id, false);
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

    int64_t nextWakeMs() const {
        int64_t at = 0;
        auto earliest = [&](int64_t t) { at = at == 0 ? t : std::min(at, t); };
        const int64_t now = steadyMs();
        for (const auto& [id, p] : loaded) {
            for (const auto& [tid, t] : p->run->timers) earliest(t.dueMs);
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
        setProblem(id, "");
        const fs::path dir = fs::path(deps.pluginsDir) / id;
        std::string source;
        if (!readFile(dir / f.manifest->main, source, 16u << 20)) {
            failedLoad.insert(id);
            recordFailure(id, f.manifest->main + " can't be read (missing, or over 16 MB)");
            return;
        }

        auto owned = std::make_unique<PluginCtx>(*this, *f.manifest, dir.string(), deps.memoryPerPlugin);
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
            state.setEnabled(id, false);
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
                   const std::function<js::Value()>& body, bool resets = true) {
        if (stopping) return;
        Call c;
        c.resets = resets;
        c.id = ++nextCall;
        c.kind = kind;
        c.what = std::move(what);
        c.budgetMs = budgetMs;
        c.startMs = steadyMs();
        c.userCaused = userCaused;
        const uint64_t id = c.id;
        p.run->calls.emplace(id, std::move(c));
        runEntry(p, id, [&] {
            js::Value r = body();
            if (Call* call = findCall(p, id)) {
                call->returned = true;
                if (r.isPromise()) call->promise = std::move(r);
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
        p.rt.setDeadline(c->deadline(steadyMs()));
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

    void succeed(PluginCtx& p, Call& c) {
        const bool counts = c.resets && (c.kind == CallKind::Load || c.kind == CallKind::Event || c.kind == CallKind::Background);
        p.run->calls.erase(c.id);
        if (counts && state.failures(p.manifest.id) > 0) state.clearFailures(p.manifest.id);
    }

    void failCall(PluginCtx& p, Call& c, const js::JsError& e) {
        const std::string text = capitalised(c.what) + " failed: " + describeError(e);
        const bool quiet = c.kind == CallKind::Cleanup;
        if (c.kind == CallKind::Load) p.run->loadFailed = true;
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
        const char* limitName = limit == Limit::Time ? "time" : limit == Limit::Stack ? "stack" : "memory";
        std::string text = "Stopped: " + what;
        if (limit == Limit::Time) text += " took longer than " + seconds(c->budgetMs);
        else if (limit == Limit::Memory) text += " used more than " + std::to_string(deps.memoryPerPlugin >> 20) + " MB of memory";
        else if (limit == Limit::Pool) text += " — plugins together used more than " + std::to_string(deps.memoryTotal >> 20) + " MB of memory";
        else text += " ran out of stack (too much recursion)";
        if (kind == CallKind::Load) p.run->loadFailed = true;
        p.run->calls.erase(callId);
        recordFailure(p.manifest.id, text);  // a stopped cleanup counts too

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
                if (t.dueMs <= now) due.emplace_back(t.dueMs, tid);
            std::sort(due.begin(), due.end());
            for (const auto& [at, tid] : due) {
                if (stopping || turnOffs.count(id)) break;
                auto t = p.run->timers.find(tid);
                if (t == p.run->timers.end()) continue;  // cleared by an earlier callback
                Timer timer = std::move(t->second);
                p.run->timers.erase(t);
                if (findCall(p, timer.owner)) {
                    runEntry(p, timer.owner, [&] { p.cx.call(timer.callback, {}); });
                } else {
                    startCall(p, CallKind::Timer, "a timer callback set by " + timer.ownerWhat, deps.handlerBudgetMs, false,
                              [&] { return p.cx.call(timer.callback, {}); });
                }
            }
        }
    }

    // ---- Commands ----------------------------------------------------------------------------

    void dispatch(const PluginEvent& ev) {
        activate();
        const nlohmann::json payload = ev.payload.is_null() ? nlohmann::json::object() : ev.payload;
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
                              [&] { return p.cx.call(handler, {js::Value::fromJson(p.cx, payload)}); });
                }
            }  // no Value of the plugin may outlive it: turning it off frees its runtime
            applyTurnOffs();
        }
    }

    void runBackground() {
        activate();
        std::vector<std::string> ids;
        for (const auto& [id, p] : loaded) ids.push_back(id);
        for (const auto& id : ids) {
            auto it = loaded.find(id);
            if (it == loaded.end() || stopping || turnOffs.count(id)) continue;
            PluginCtx& p = *it->second;
            if (p.run->background.isUndefined()) continue;
            {
                const js::Value task = p.run->background;
                startCall(p, CallKind::Background, "the background task", deps.backgroundBudgetMs, false,
                          [&] { return p.cx.call(task, {}); });
            }
            applyTurnOffs();
        }
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
            if (loaded.count(f.dir)) reloadNow.push_back(f.dir);
            unload(f.dir);
            failedLoad.erase(f.dir);
            setProblem(f.dir, "");
        }
        if (only) failedLoad.erase(*only);
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
    inbox->post([serial = ctxSerial_, id = id_, settle = std::move(settle)](PluginHost::Impl& h) {
        PluginCtx* p = h.bySerial(serial);
        if (!p) return;
        auto it = p->run->work.find(id);
        if (it == p->run->work.end()) return;  // cancelled meanwhile
        PendingWork w = std::move(it->second);
        p->run->work.erase(it);
        auto body = [&] {
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
        if (h.findCall(*p, w.owner)) {
            h.runEntry(*p, w.owner, body);
        } else {
            h.startCall(*p, CallKind::Continued, w.ownerWhat + ", continuing after it ended", h.deps.handlerBudgetMs, false,
                        [&] {
                            body();
                            return js::Value();
                        });
        }
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
    auto owner = p.run->calls.find(p.run->current);
    const bool cleanup = owner != p.run->calls.end() && owner->second.kind == CallKind::Cleanup;
    Timer t;
    t.dueMs = steadyMs() + delayMs;
    // A stopped call's cleanup code owns nothing: its timer runs as a call of its own.
    t.owner = cleanup ? 0 : p.run->current;
    t.ownerWhat = owner == p.run->calls.end() ? "the plugin" : owner->second.what;
    t.callback = std::move(callback);
    const uint64_t id = ++h.nextTimer;
    p.run->timers.emplace(id, std::move(t));
    return id;
}

void clearTimer(PluginCtx& p, uint64_t id) { p.run->timers.erase(id); }

void addHandler(PluginCtx& p, const std::string& event, js::Value handler) { p.run->handlers[event].push_back(std::move(handler)); }

void setBackgroundTask(PluginCtx& p, js::Value task) { p.run->background = std::move(task); }

}  // namespace plugins

// ---- PluginHost ---------------------------------------------------------------------------------

PluginHost::PluginHost(PluginHostDeps deps) : impl_(std::make_unique<Impl>(std::move(deps))) {}

PluginHost::~PluginHost() { stop(); }

namespace {
void* threadMain(void* impl) {
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

void PluginHost::runBackground() {
    impl_->post([](Impl& h) { h.runBackground(); });
}

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
        s.loaded = h.loadedIds.count(f.dir) > 0;
        s.badge = h.state.badge(f.dir);
        s.errors = h.state.lastErrors(f.dir);
        if (auto p = h.problems.find(f.dir); p != h.problems.end()) s.errors.push_back(p->second);
        out.push_back(std::move(s));
    }
    return out;
}

void PluginHost::setEnabled(const std::string& id, bool on) {
    impl_->state.setEnabled(id, on);
    if (on) impl_->state.clearFailures(id);
    impl_->post([id, on](Impl& h) {
        h.failedLoad.erase(id);
        h.setProblem(id, "");
        if (!on) h.unload(id);
    });
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
