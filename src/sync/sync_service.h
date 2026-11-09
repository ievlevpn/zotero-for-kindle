#pragma once
// The sync worker thread (spec §7.2): Zotero sync, WebDAV downloads and on-demand annotations,
// with its own database connection. The UI thread sends commands and polls events; it never
// waits for the network. stop() returns within ~2 s even mid-request or mid-Retry-After.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "net/http.h"
#include "platform/power.h"
#include "sync/config.h"
#include "sync/sync_control.h"

namespace zk {

struct SyncDeps {
    std::function<std::unique_ptr<Http>()> makeHttp;
    std::string dbPath, filesDir;
    Config config;
    KeepAwake* keepAwake = nullptr;
    int intervalMinutes = 10;
    std::function<bool()> wifiUp;  // default: probeNetState() (lipc on the Kindle), on the worker
    int onlinePollMs = 5000;
    int onlineGiveUpMs = 120'000;
    int pushDelayMs = 10'000;  // pushSoon(): this long after the last change
    // Whether pulls collect events for plugins (asked on the worker; must be cheap and thread-safe):
    // false while no plugin is on. Unset: always.
    std::function<bool()> pluginEventsWanted;
};

class SyncService final : public SyncControl {
public:
    explicit SyncService(SyncDeps);
    ~SyncService() override;
    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    void start();
    void stop();

    void syncNow() override;
    void pushSoon() override;
    void syncWhenOnline() override;
    void openBook(const std::string& itemKey, const std::string& attachmentKey) override;
    void setAllHighlightsOffline(bool) override;
    void removeDownloads() override;
    void setIntervalMinutes(int) override;
    bool poll(SyncEvent& out) override;
    bool busy() const override { return busy_; }

private:
    struct Command {
        enum class Kind { Sync, SyncWhenOnline, OpenBook, SetAllOffline, RemoveDownloads, Push } kind;
        std::string itemKey, attachmentKey;
        bool flag = false;
    };
    struct Worker;

    void enqueue(Command);
    void run();
    void emit(SyncEvent);
    bool sleepFor(int seconds);  // false when stopping
    bool sleepForMs(int ms);
    void pushChanges(Worker&);  // PushStarted/PushFinished around SyncEngine::push, if anything is unsent

    SyncDeps deps_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    std::deque<SyncEvent> events_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> stopping_{false};
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    int intervalMinutes_;
    std::optional<std::chrono::steady_clock::time_point> pushDue_;  // guarded by mutex_
};

}  // namespace zk
