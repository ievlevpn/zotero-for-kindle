#include "sync/sync_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "core/log.h"
#include "net/write_guard.h"
#include "store/db.h"
#include "store/library.h"
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
    std::unique_ptr<SyncEngine> engine;
    std::unique_ptr<FileStore> files;  // WebDAV or Zotero storage
};

SyncService::SyncService(SyncDeps deps) : deps_(std::move(deps)), intervalMinutes_(deps_.intervalMinutes) {
    if (!deps_.wifiUp) deps_.wifiUp = [] { return probeNetState().wifiConnected; };
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

void SyncService::enqueue(Command c) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((c.kind == Command::Kind::Sync || c.kind == Command::Kind::SyncWhenOnline) &&
            std::any_of(commands_.begin(), commands_.end(),
                        [](const Command& q) { return q.kind == Command::Kind::Sync || q.kind == Command::Kind::SyncWhenOnline; }))
            return;  // one pending sync is enough
        // Opening a book is what the reader is waiting for: ahead of queued syncs.
        if (c.kind == Command::Kind::OpenBook) commands_.push_front(std::move(c));
        else commands_.push_back(std::move(c));
    }
    cv_.notify_all();
}

void SyncService::syncNow() { enqueue({Command::Kind::Sync, {}, {}, false}); }
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
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(e));
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
        busy_ = true;
        std::optional<KeepAwakeScope> awake;
        if (deps_.keepAwake) awake.emplace(*deps_.keepAwake);
        try {
            if (cmd.kind == Command::Kind::SyncWhenOnline) {
                // Wi-Fi comes back a few seconds after wake; wait for it here, quietly.
                const auto until = Clock::now() + std::chrono::milliseconds(deps_.onlineGiveUpMs);
                bool online = deps_.wifiUp();
                while (!online && !stopping_ && Clock::now() < until && sleepForMs(deps_.onlinePollMs)) online = deps_.wifiUp();
                if (!online || stopping_) {
                    busy_ = false;
                    continue;
                }
                cmd.kind = Command::Kind::Sync;
            }
            switch (cmd.kind) {
                case Command::Kind::SyncWhenOnline: break;  // turned into Sync above
                case Command::Kind::Push:
                    pushChanges(w);
                    break;
                case Command::Kind::Sync: {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        pushDue_.reset();  // this sync sends them
                    }
                    SyncEvent start;
                    start.kind = SyncEvent::Kind::SyncStarted;
                    emit(start);
                    pushChanges(w);  // ours first, so a pull can't make them look like conflicts
                    SyncReport rep = w.engine->pull([this](const SyncProgress& p) {
                        SyncEvent e;
                        e.kind = SyncEvent::Kind::SyncProgress;
                        e.progress = p;
                        emit(std::move(e));
                    });
                    lastSync = Clock::now();
                    if (!rep.error && !stopping_) pushChanges(w);  // e.g. conflicts re-applied meanwhile
                    SyncEvent done;
                    done.kind = SyncEvent::Kind::SyncFinished;
                    done.error = rep.error;
                    done.report = std::move(rep);  // may carry thousands of plugin events: not copied
                    if (!stopping_) emit(std::move(done));
                    break;
                }
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
                        SyncEvent p0;
                        p0.kind = SyncEvent::Kind::DownloadProgress;
                        p0.itemKey = cmd.itemKey;
                        p0.attachmentKey = cmd.attachmentKey;
                        emit(p0);
                        auto last = Clock::now();
                        int lastPct = 0;
                        w.files->setProgress([&](int64_t d, int64_t t) {
                            const int pct = t > 0 ? static_cast<int>(100 * d / t) : 0;
                            if (pct - lastPct < 5 && Clock::now() - last < std::chrono::seconds(1)) return;
                            lastPct = pct;
                            last = Clock::now();
                            SyncEvent e = p0;
                            e.done = d;
                            e.total = t;
                            emit(std::move(e));
                        });
                        UserError err;
                        if (w.files->download(*att, deps_.filesDir, &err)) {
                            const std::string path = (fs::path(deps_.filesDir) / att->key / localFileName(*att)).string();
                            w.lib->setFileState(att->key, AttachmentRecord::State::Local, path);
                        } else {
                            done.error = err;
                        }
                        w.files->setProgress(nullptr);
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
            ev.itemKey = cmd.itemKey;
            ev.attachmentKey = cmd.attachmentKey;
            ev.error = w.lib->lastError();
            emit(std::move(ev));
        }
        busy_ = false;
    }
    log(LogLevel::Info, "sync service: stopped");
}

void SyncService::pushChanges(Worker& w) {
    const int pending = w.engine->pendingChanges();
    if (pending == 0 || stopping_) return;
    SyncEvent start;
    start.kind = SyncEvent::Kind::PushStarted;
    start.total = pending;
    emit(start);
    SyncReport rep = w.engine->push();
    SyncEvent done;
    done.kind = SyncEvent::Kind::PushFinished;
    done.error = rep.error;
    done.report = std::move(rep);
    if (!stopping_) emit(std::move(done));
}

}  // namespace zk
