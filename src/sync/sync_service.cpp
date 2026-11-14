#include "sync/sync_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>

#include "core/free_space.h"
#include "core/log.h"
#include <nlohmann/json.hpp>
#include "core/wake.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/libraries.h"
#include "sync/sync_engine.h"
#include "sync/webdav.h"
#include "sync/zotero_storage.h"
#include "zotero/api.h"

namespace zk {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// Puts the service's cancel flag on every request and refuses new ones once it is set.
class CancellableHttp final : public Http {
public:
    CancellableHttp(Http& inner, std::shared_ptr<std::atomic<bool>> flag) : inner_(inner), flag_(std::move(flag)) {}
    HttpResponse send(const HttpRequest& req) override {
        if (*flag_) {
            HttpResponse r;
            r.curlCode = 42;  // CURLE_ABORTED_BY_CALLBACK
            r.curlError = "cancelled";
            return r;
        }
        HttpRequest copy = req;
        copy.cancel = flag_;
        return inner_.send(copy);
    }

private:
    Http& inner_;
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace

// Everything that lives on the worker thread.
struct SyncService::Worker {
    std::unique_ptr<Http> http;
    std::unique_ptr<CancellableHttp> cancellable;
    std::unique_ptr<Db> db;
    std::unique_ptr<Library> lib;
    std::unique_ptr<WriteGuard> guard;
    std::unique_ptr<ZoteroApi> api;
    std::unique_ptr<SyncEngine> engine;  // My Library's (on-demand annotations find their file's library)
    std::map<int64_t, std::unique_ptr<SyncEngine>> groupEngines;
    SyncEngine& engineFor(int64_t lib, const std::function<bool()>& eventsWanted) {
        if (lib == kMyLibrary) return *engine;
        auto& e = groupEngines[lib];
        if (!e) {
            e = std::make_unique<SyncEngine>(*api, *this->lib, lib);
            e->setPluginEventsWanted(eventsWanted);
        }
        return *e;
    }
    std::unique_ptr<FileStore> files;  // WebDAV or Zotero storage
    // The kept collections' catch-up, listed once when it starts: the files still to download
    // (each with the kept collections it is in), and per collection how many it lacked then.
    struct KeptFile {
        std::string itemKey;
        AttachmentRecord att;
        std::set<std::string> collections;
    };
    std::vector<KeptFile> keptQueue;
    std::map<std::string, int> keptTotal;
};

SyncService::SyncService(SyncDeps deps) : deps_(std::move(deps)), intervalMinutes_(deps_.intervalMinutes) {
    if (!deps_.wifiUp) deps_.wifiUp = [] { return probeNetState().wifiConnected; };
    if (!deps_.wallMs)
        deps_.wallMs = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        };
    if (!deps_.freeBytes)
        deps_.freeBytes = [dir = deps_.filesDir] {
            fs::path p = dir;  // the files folder, or the nearest folder there is (before the first download)
            std::error_code ec;
            while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p.parent_path() != p) p = p.parent_path();
            return freeBytesAt(p.string());
        };
}

SyncService::~SyncService() { stop(); }

void SyncService::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    *cancel_ = false;
    thread_ = std::thread([this] { run(); });
}

void SyncService::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        *cancel_ = true;
        commands_.clear();
    }
    cv_.notify_all();
    thread_.join();
}

uint64_t SyncService::enqueue(Command c) {
    uint64_t request = c.request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (c.kind == Command::Kind::Sync || c.kind == Command::Kind::SyncWhenOnline)
            for (auto& q : commands_)
                if (q.kind == Command::Kind::Sync || q.kind == Command::Kind::SyncWhenOnline) {
                    if (q.request == 0) q.request = c.request;
                    return q.request;  // one pending sync is enough: the asker waits for that one
                }
        if (c.kind == Command::Kind::KeepDownloads)
            for (auto& q : commands_)
                if (q.kind == Command::Kind::KeepDownloads) {
                    q.flag = q.flag || c.flag;
                    return 0;  // one pending is enough
                }
        // Opening a book is what the reader is waiting for: ahead of queued syncs and kept
        // downloads, and a kept file under way gives way to it.
        if (c.kind == Command::Kind::OpenBook && keptRunning_) {
            keptYield_ = true;
            *cancel_ = true;
        }
        if (c.kind == Command::Kind::OpenBook) commands_.push_front(std::move(c));
        else commands_.push_back(std::move(c));
    }
    cv_.notify_all();
    return request;
}

void SyncService::syncNow() { enqueue({Command::Kind::Sync, {}, {}, false}); }
uint64_t SyncService::syncNowTracked() {
    Command c{Command::Kind::Sync, {}, {}, false};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        c.request = ++nextRequest_;
    }
    return enqueue(std::move(c));
}
void SyncService::pushSoon() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pushDue_ = Clock::now() + std::chrono::milliseconds(deps_.pushDelayMs);
    }
    cv_.notify_all();
}
void SyncService::syncWhenOnline() { enqueue({Command::Kind::SyncWhenOnline, {}, {}, false}); }
void SyncService::openBook(const std::string& itemKey, const std::string& attachmentKey) {
    enqueue({Command::Kind::OpenBook, itemKey, attachmentKey, false});
}
void SyncService::setAllHighlightsOffline(bool on) { enqueue({Command::Kind::SetAllOffline, {}, {}, on}); }
void SyncService::removeDownloads() { enqueue({Command::Kind::RemoveDownloads, {}, {}, false}); }
void SyncService::downloadKept() { enqueue({Command::Kind::KeepDownloads, {}, {}, true}); }
void SyncService::removeLibrary(int64_t lib, bool evenUnsent) {
    enqueue({Command::Kind::RemoveLibrary, {}, {}, evenUnsent, lib});
}

std::optional<KeptStatus> SyncService::keptStatus(const std::string& collectionKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = kept_.find(collectionKey);
    if (it == kept_.end()) return std::nullopt;
    return it->second;
}

void SyncService::setKept(std::map<std::string, KeptStatus> status) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status == kept_) return;
        kept_ = std::move(status);
    }
    SyncEvent e;
    e.kind = SyncEvent::Kind::KeptChanged;
    emit(std::move(e));
}

void SyncService::setIntervalMinutes(int m) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        intervalMinutes_ = m;
    }
    cv_.notify_all();
}

bool SyncService::poll(SyncEvent& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

void SyncService::emit(SyncEvent e) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(e));
    }
    wakeUi();  // the loop polls the events only when it runs
}

bool SyncService::sleepForMs(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping_.load(); });
    return !stopping_;
}

bool SyncService::sleepFor(int seconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::seconds(seconds), [this] { return stopping_.load(); });
    return !stopping_;
}

void SyncService::run() {
    Worker w;
    try {
        w.http = deps_.makeHttp();
        w.cancellable = std::make_unique<CancellableHttp>(*w.http, cancel_);
        w.db = std::make_unique<Db>(Db::open(deps_.dbPath));
        w.lib = std::make_unique<Library>(*w.db);
        Library* lib = w.lib.get();
        w.guard = std::make_unique<WriteGuard>(
            *w.cancellable, [lib](const std::string& key) { return lib->annotation(key).has_value(); },
            [lib](const std::string& key) { return lib->attachment(key).has_value(); },  // reading positions
            [lib](const std::string& key) -> std::optional<std::vector<std::string>> {   // the read tag
                if (auto item = lib->zoteroItem(key)) return item->tags;  // as Zotero has them
                return std::nullopt;
            },
            pluginChangeShapes(*lib));  // plugin changes, each in its pending shape
        // Group libraries: each object only to its own library's path, and only to groups the Kindle may edit.
        w.guard->setLibraries([lib](const std::string& key) { return lib->libraryOf(key); },
                              [lib](int64_t group) { return lib->libraryEditable(group); },
                              [lib](const std::string& key) { return lib->annotationEditable(key); });
        if (lib->userId() == 0 && deps_.config.userId > 0) lib->setUser(deps_.config.userId, "");
        w.api = std::make_unique<ZoteroApi>(*w.guard, deps_.config.userId, deps_.config.apiKey);
        w.api->setSleeper([this](int s) { sleepFor(s); });
        w.engine = std::make_unique<SyncEngine>(*w.api, *w.lib);
        w.engine->setPluginEventsWanted(deps_.pluginEventsWanted);
        w.files = makeFileStore(*w.guard, deps_.config);
    } catch (const DbError& e) {
        log(LogLevel::Error, "sync service: can't open the database: %s", e.what());
        SyncEvent ev;
        ev.kind = SyncEvent::Kind::SyncFinished;
        ev.error = UserError{ErrorKind::Other, "Can't open the library database.", e.what()};
        emit(std::move(ev));
        return;
    }

    auto lastSync = Clock::now();
    while (!stopping_) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            auto due = [&] {
                return intervalMinutes_ > 0 && Clock::now() - lastSync >= std::chrono::minutes(intervalMinutes_);
            };
            auto pushDue = [&] { return pushDue_ && Clock::now() >= *pushDue_; };
            // Up to 30 s (the interval check), or until a pending push is due; pushSoon() may move
            // that deadline while we wait, so it is read again on every wake-up.
            const auto giveUp = Clock::now() + std::chrono::seconds(30);
            while (!(stopping_ || !commands_.empty() || due() || pushDue())) {
                auto wake = giveUp;
                if (pushDue_ && *pushDue_ < wake) wake = *pushDue_;
                if (cv_.wait_until(lock, wake) == std::cv_status::timeout && Clock::now() >= giveUp) break;
            }
            if (stopping_) break;
            if (commands_.empty()) {
                if (pushDue()) {
                    pushDue_.reset();
                    cmd = {Command::Kind::Push, {}, {}, false};
                } else if (due()) {
                    cmd = {Command::Kind::Sync, {}, {}, false};
                } else {
                    continue;
                }
            } else {
                cmd = std::move(commands_.front());
                commands_.pop_front();
            }
        }
        busy_ = cmd.kind != Command::Kind::KeepDownloads;  // a book opened meanwhile goes first: not "waiting"
        std::optional<KeepAwakeScope> awake;
        try {
            if (cmd.kind == Command::Kind::SyncWhenOnline) {
                // Wi-Fi comes back a few seconds after wake; wait for it here, quietly. Without
                // keep-awake: with Wi-Fi off (many readers keep it off) the wait runs out, and
                // holding the screensaver off for it kept the Kindle awake 2 minutes per wake.
                const auto until = Clock::now() + std::chrono::milliseconds(deps_.onlineGiveUpMs);
                bool online = deps_.wifiUp();
                while (!online && !stopping_ && Clock::now() < until && sleepForMs(deps_.onlinePollMs)) online = deps_.wifiUp();
                if (!online || stopping_) {
                    if (cmd.request != 0 && !stopping_) {  // someone waits for this sync's end: it didn't happen
                        SyncEvent done;
                        done.kind = SyncEvent::Kind::SyncFinished;
                        done.request = cmd.request;
                        done.error = UserError{ErrorKind::NoWifi, "No Wi-Fi. Connect to a network and try again.", "offline"};
                        emit(std::move(done));
                    }
                    busy_ = false;
                    continue;
                }
                cmd.kind = Command::Kind::Sync;
            }
            // Online (or not waiting): the work itself. Not kept downloads: they never hold the
            // Kindle awake beyond the sync before them (if it sleeps, they stop with Wi-Fi).
            if (deps_.keepAwake && cmd.kind != Command::Kind::KeepDownloads) awake.emplace(*deps_.keepAwake);
            switch (cmd.kind) {
                case Command::Kind::SyncWhenOnline: break;  // turned into Sync above
                case Command::Kind::Push: {
                    std::vector<int64_t> passed;
                    const SyncReport pushed = pushChanges(w, &passed);
                    recordGroupErrors(*w.lib, passed, pushed);
                    break;
                }
                case Command::Kind::Sync: {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        pushDue_.reset();  // this sync sends them
                    }
                    SyncEvent start;
                    start.kind = SyncEvent::Kind::SyncStarted;
                    start.request = cmd.request;
                    emit(start);
                    // Which groups there are, and who may edit them (one request when nothing changed).
                    UserError discovery;
                    if (!discoverGroups(*w.api, *w.lib, deps_.wallMs(), &discovery) && unreachable(discovery)) {
                        // Zotero can't be reached: every other request would fail the same way.
                        lastSync = Clock::now();
                        SyncEvent done;
                        done.kind = SyncEvent::Kind::SyncFinished;
                        done.request = cmd.request;
                        done.error = discovery;
                        done.report.error = discovery;
                        if (!stopping_) emit(std::move(done));
                        break;
                    }
                    std::vector<int64_t> passed;  // groups gone through: their errors are recorded
                    SyncReport pushed = pushChanges(w, &passed);  // ours first, so a pull can't make them look like conflicts
                    auto progress = [this](const SyncProgress& p) {
                        SyncEvent e;
                        e.kind = SyncEvent::Kind::SyncProgress;
                        e.progress = p;
                        emit(std::move(e));
                    };
                    // My Library, then each group switched on and still there ("Syncing <group>…").
                    // A group's failure is its own (groupErrors): the others and the last push go on.
                    SyncReport rep = w.engine->pull(progress);
                    if (!rep.error)
                        for (int64_t group : w.lib->syncedGroups()) {
                            if (stopping_) break;
                            SyncReport one = w.engineFor(group, deps_.pluginEventsWanted).pull(progress);
                            scopeGroupError(one, group, *w.lib);
                            if (std::find(passed.begin(), passed.end(), group) == passed.end()) passed.push_back(group);
                            mergeReport(rep, std::move(one));
                        }
                    lastSync = Clock::now();
                    if (!rep.error && !stopping_) {
                        SyncReport again = pushChanges(w, &passed);  // e.g. conflicts re-applied meanwhile
                        rep.groupErrors.insert(rep.groupErrors.end(), again.groupErrors.begin(), again.groupErrors.end());
                    }
                    rep.groupErrors.insert(rep.groupErrors.begin(), pushed.groupErrors.begin(), pushed.groupErrors.end());
                    recordGroupErrors(*w.lib, passed, rep);
                    SyncEvent done;
                    done.kind = SyncEvent::Kind::SyncFinished;
                    done.request = cmd.request;
                    done.error = rep.error;
                    done.report = std::move(rep);  // may carry thousands of plugin events: not copied
                    const bool ok = !done.error;
                    if (!stopping_) emit(std::move(done));
                    if (ok && !stopping_) enqueue({Command::Kind::KeepDownloads, {}, {}, true});  // new files of kept collections
                    break;
                }
                case Command::Kind::KeepDownloads:
                    if (keepStep(w, cmd.flag) && !stopping_) enqueue({Command::Kind::KeepDownloads, {}, {}, false});
                    break;
                case Command::Kind::OpenBook: {
                    SyncEvent done;
                    done.kind = SyncEvent::Kind::DownloadFinished;
                    done.itemKey = cmd.itemKey;
                    done.attachmentKey = cmd.attachmentKey;
                    auto att = w.lib->attachment(cmd.attachmentKey);
                    const bool local = att && att->state == AttachmentRecord::State::Local && fs::exists(att->localPath);
                    if (!att) {
                        done.error = UserError{ErrorKind::Other, "This file isn't in the library yet. Sync, then try again.",
                                               "no attachment " + cmd.attachmentKey};
                    } else if (!local) {
                        UserError err;
                        if (!download(w, cmd.itemKey, *att, &err)) done.error = err;
                    }
                    const bool ok = !done.error;
                    if (!stopping_) emit(done);
                    if (ok && !stopping_) {
                        SyncReport rep = w.engine->pullAnnotationsFor(cmd.attachmentKey, nullptr);
                        SyncEvent ready;
                        ready.kind = SyncEvent::Kind::AnnotationsReady;
                        ready.itemKey = cmd.itemKey;
                        ready.attachmentKey = cmd.attachmentKey;
                        ready.report = rep;
                        ready.error = rep.error;
                        if (!stopping_) emit(std::move(ready));
                    }
                    break;
                }
                case Command::Kind::SetAllOffline:
                    w.engine->setAllHighlightsOffline(cmd.flag);
                    if (cmd.flag) syncNow();
                    break;
                case Command::Kind::RemoveLibrary: {
                    // Its kept and pinned collections go with it: nothing of it comes back unasked.
                    for (const auto& key : w.lib->keptCollections())
                        if (w.lib->libraryOf(key) == cmd.lib) w.lib->setCollectionKept(key, false);
                    for (const auto& key : w.lib->pinnedCollections())
                        if (w.lib->libraryOf(key) == cmd.lib) w.lib->setCollectionPinned(key, false);
                    const LibraryRemoval r = w.lib->removeLibraryData(cmd.lib, cmd.flag);
                    log(LogLevel::Info, "sync service: group %lld %s (%d files, %d unsent)", static_cast<long long>(cmd.lib),
                        r.removed ? "removed from the Kindle" : "not removed", r.downloaded, r.unsent);
                    SyncEvent done;
                    done.kind = SyncEvent::Kind::SyncFinished;
                    emit(std::move(done));
                    break;
                }
                case Command::Kind::RemoveDownloads: {
                    for (const auto& a : w.lib->downloadedAttachments()) {
                        std::error_code ec;
                        fs::remove_all(fs::path(deps_.filesDir) / a.key, ec);  // our own downloaded copy
                        w.lib->setFileState(a.key, AttachmentRecord::State::Remote, "");
                    }
                    SyncEvent done;
                    done.kind = SyncEvent::Kind::SyncFinished;
                    emit(std::move(done));
                    break;
                }
            }
        } catch (const DbError& e) {
            log(LogLevel::Error, "sync service: %s", e.what());
            SyncEvent ev;
            ev.kind = cmd.kind == Command::Kind::OpenBook ? SyncEvent::Kind::DownloadFinished : SyncEvent::Kind::SyncFinished;
            if (cmd.kind == Command::Kind::Sync) ev.request = cmd.request;
            ev.itemKey = cmd.itemKey;
            ev.attachmentKey = cmd.attachmentKey;
            ev.error = w.lib->lastError();
            emit(std::move(ev));
        }
        busy_ = false;
    }
    log(LogLevel::Info, "sync service: stopped");
}

bool SyncService::download(Worker& w, const std::string& itemKey, const AttachmentRecord& att, UserError* err,
                           const std::function<void(int64_t)>& onTotal) {
    SyncEvent p0;
    p0.kind = SyncEvent::Kind::DownloadProgress;
    p0.itemKey = itemKey;
    p0.attachmentKey = att.key;
    emit(p0);
    auto last = Clock::now();
    int lastPct = 0;
    bool totalSeen = false;
    w.files->setProgress([&](int64_t d, int64_t t) {
        if (onTotal && !totalSeen && t > 0) {
            totalSeen = true;
            onTotal(t);
        }
        const int pct = t > 0 ? static_cast<int>(100 * d / t) : 0;
        if (pct - lastPct < 5 && Clock::now() - last < std::chrono::seconds(1)) return;
        lastPct = pct;
        last = Clock::now();
        SyncEvent e = p0;
        e.done = d;
        e.total = t;
        emit(std::move(e));
    });
    const bool ok = w.files->download(att, deps_.filesDir, err);
    w.files->setProgress(nullptr);
    if (ok) w.lib->setFileState(att.key, AttachmentRecord::State::Local, (fs::path(deps_.filesDir) / att.key / localFileName(att)).string());
    return ok;
}

namespace {

constexpr int64_t kKeptRetryMs = int64_t{7} * 24 * 3600 * 1000;  // a file that failed: tried again after a week

// Files that failed for reasons of their own (not on the server, wrong checksum), kept in the
// library meta across syncs: {key: {ms, version}}. Tried again after kKeptRetryMs, or once the
// attachment changed in Zotero.
nlohmann::json keptFailures(Library& lib) {
    auto j = nlohmann::json::parse(lib.metaValue("keptFailures"), nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

bool failedRecently(const nlohmann::json& failures, const std::string& key, int64_t version, int64_t nowMs) {
    auto it = failures.find(key);
    if (it == failures.end() || !it->is_object()) return false;
    return it->value("version", int64_t{-1}) == version && nowMs - it->value("ms", int64_t{0}) < kKeptRetryMs;
}

}  // namespace

// One step of the kept collections' catch-up (spec B). A fresh start lists what they miss; each
// step drops collections no longer kept, then downloads one file. It stops quietly without
// Wi-Fi (never turned on for it) or on any failure not about the file itself (the next sync
// tries again), pauses under kKeptMinFree (also when the file's size would take it there), and
// gives way to a book being opened (enqueue() cancels the file under way; it comes again).
bool SyncService::keepStep(Worker& w, bool fresh) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        keptRunning_ = true;  // from here: an open during the lookup gives way too
    }
    struct Done {
        SyncService& s;
        bool cancelReset = false;
        ~Done() {
            std::lock_guard<std::mutex> lock(s.mutex_);
            s.keptRunning_ = false;
            if ((s.keptYield_ || cancelReset) && !s.stopping_) *s.cancel_ = false;  // the cancel was for this file only
            s.keptYield_ = false;
        }
    } done{*this};
    auto stop = [&](std::map<std::string, KeptStatus> status) {
        w.keptQueue.clear();
        w.keptTotal.clear();
        setKept(std::move(status));
        return false;
    };

    const std::vector<std::string> kept = w.lib->keptCollections();
    if (fresh) {
        w.keptQueue.clear();
        w.keptTotal.clear();
        const auto failures = keptFailures(*w.lib);
        const int64_t now = deps_.wallMs();
        for (const auto& key : kept)
            for (const ItemRow& row : w.lib->collectionFiles(key)) {
                auto att = w.lib->attachment(row.attachmentKey);
                if (!att || (att->state == AttachmentRecord::State::Local && fs::exists(att->localPath))) continue;
                if (failedRecently(failures, att->key, w.lib->objectVersion(att->key), now)) continue;
                auto it = std::find_if(w.keptQueue.begin(), w.keptQueue.end(),
                                       [&](const Worker::KeptFile& f) { return f.att.key == att->key; });
                if (it == w.keptQueue.end()) it = w.keptQueue.insert(w.keptQueue.end(), {row.key, *att, {}});
                it->collections.insert(key);
                ++w.keptTotal[key];
            }
    } else {  // keeping turned off meanwhile, or its group hidden: those collections' files are no longer wanted
        std::set<std::string> keptSet;
        for (const auto& key : kept)
            if (w.lib->libraryShown(w.lib->libraryOf(key).value_or(kMyLibrary))) keptSet.insert(key);
        for (auto& f : w.keptQueue)
            for (auto c = f.collections.begin(); c != f.collections.end();) c = keptSet.count(*c) ? std::next(c) : f.collections.erase(c);
        std::erase_if(w.keptQueue, [](const Worker::KeptFile& f) { return f.collections.empty(); });
        std::erase_if(w.keptTotal, [&](const auto& t) { return !keptSet.count(t.first); });
    }
    if (w.keptQueue.empty() || stopping_) return stop({});
    if (!deps_.wifiUp()) return stop({});  // quietly; the next sync (Wi-Fi up again) carries on

    std::map<std::string, KeptStatus> status;  // "Downloading <current> of <total>"
    for (const auto& [key, total] : w.keptTotal) {
        const int left = static_cast<int>(std::count_if(w.keptQueue.begin(), w.keptQueue.end(),
                                                         [&](const Worker::KeptFile& f) { return f.collections.count(key) > 0; }));
        if (left > 0) status[key] = {total - left + 1, total, false};
    }
    auto paused = [&] {
        for (auto& [key, st] : status) st.paused = true;
        return stop(status);  // the next sync looks again
    };
    if (deps_.freeBytes() < kKeptMinFree) return paused();
    setKept(status);

    const Worker::KeptFile next = w.keptQueue.front();
    // Opened (and so downloaded) meanwhile?
    if (auto att = w.lib->attachment(next.att.key); !att || (att->state == AttachmentRecord::State::Local && fs::exists(att->localPath))) {
        w.keptQueue.erase(w.keptQueue.begin());
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (keptYield_) return true;  // a book was opened during the lookup: it goes first
    }
    // Once the size is known: would it take the Kindle under the floor? (WebDAV keeps the zip and
    // the file it holds, so twice the size.)
    bool tooBig = false;
    const uint64_t factor = deps_.config.webdavUrl.empty() || next.att.lib != kMyLibrary ? 1 : 2;
    auto onTotal = [&](int64_t total) {
        const uint64_t need = static_cast<uint64_t>(total) * factor, free = deps_.freeBytes();
        if (free >= need && free - need >= kKeptMinFree) return;
        tooBig = true;
        std::lock_guard<std::mutex> lock(mutex_);
        *cancel_ = true;
        done.cancelReset = true;
    };
    UserError err;
    const bool ok = download(w, next.itemKey, next.att, &err, onTotal);
    bool yielded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        yielded = keptYield_;
    }
    if (stopping_) return stop({});
    if (ok) {
        if (auto failures = keptFailures(*w.lib); failures.erase(next.att.key))
            w.lib->setMetaValue("keptFailures", failures.dump());
        w.keptQueue.erase(w.keptQueue.begin());
        SyncEvent fin;
        fin.kind = SyncEvent::Kind::DownloadFinished;
        fin.itemKey = next.itemKey;
        fin.attachmentKey = next.att.key;
        emit(std::move(fin));
        return true;
    }
    if (tooBig) return paused();
    if (yielded) return true;  // the opened book first; this file again after it
    log(LogLevel::Info, "kept: %s not downloaded: %s", next.att.key.c_str(), err.details.c_str());
    if (err.kind == ErrorKind::NotOnServer || err.kind == ErrorKind::Checksum) {  // the file's own fault
        auto failures = keptFailures(*w.lib);
        failures[next.att.key] = {{"ms", deps_.wallMs()}, {"version", w.lib->objectVersion(next.att.key)}};
        w.lib->setMetaValue("keptFailures", failures.dump());
        w.keptQueue.erase(w.keptQueue.begin());
        return true;
    }
    if (err.kind == ErrorKind::DiskFull) return paused();
    return stop({});  // network, server, login: not file after file; the next sync tries again
}

SyncReport SyncService::pushChanges(Worker& w, std::vector<int64_t>* passed) {
    // Plugin changes aimed at groups they can never go to (read-only, switched off, gone,
    // unreadable) are refused first, whatever else is pending: the plugin hears it now.
    const int refused = w.lib->refuseUnsendablePluginChanges();
    // Every library with something to send: My Library first, then the groups. pendingChanges
    // counts what push() will really send.
    std::vector<int64_t> libs;
    int pending = 0;
    for (int64_t l : librariesToSync(*w.lib))
        if (const int n = w.engineFor(l, deps_.pluginEventsWanted).pendingChanges(); n > 0) {
            pending += n;
            libs.push_back(l);
        }
    SyncReport rep;
    rep.pluginRefused = refused;
    if (pending == 0 || stopping_) return rep;
    SyncEvent start;
    start.kind = SyncEvent::Kind::PushStarted;
    start.total = pending;
    emit(start);
    for (size_t i = 0; i < libs.size() && !stopping_; ++i) {
        SyncReport one = w.engineFor(libs[i], deps_.pluginEventsWanted).push();
        if (libs[i] != kMyLibrary) {
            scopeGroupError(one, libs[i], *w.lib);  // a group's refusal is the group's, not the account's
            if (passed && std::find(passed->begin(), passed->end(), libs[i]) == passed->end()) passed->push_back(libs[i]);
        }
        if (i == 0) {
            const int r = rep.pluginRefused;
            rep = std::move(one);
            rep.pluginRefused += r;
        } else {
            mergeReport(rep, std::move(one));
        }
    }
    SyncEvent done;
    done.kind = SyncEvent::Kind::PushFinished;
    done.error = rep.error;
    done.report = rep;
    if (!stopping_) emit(std::move(done));
    rep.pluginEvents.clear();  // told with PushFinished
    return rep;
}

}  // namespace zk
