#pragma once
// The sync worker thread (spec §7.2): Zotero sync, WebDAV downloads and on-demand annotations,
// with its own database connection. The UI thread sends commands and polls events; it never
// waits for the network. stop() returns within ~2 s even mid-request or mid-Retry-After.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "net/http.h"
#include "platform/power.h"
#include "store/records.h"
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
    // Free bytes where the files go (kept collections pause under kKeptMinFree). Default: statvfs.
    std::function<uint64_t()> freeBytes;
    std::function<int64_t()> wallMs;  // ms since the epoch (a failed kept file's retry); default: the clock
};

// Kept collections stop downloading below this much free space (spec B).
constexpr uint64_t kKeptMinFree = uint64_t{300} << 20;

class SyncService final : public SyncControl {
public:
    explicit SyncService(SyncDeps);
    ~SyncService() override;
    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    void start();
    void stop();

    void syncNow() override;
    uint64_t syncNowTracked() override;
    void pushSoon() override;
    void syncWhenOnline() override;
    void openBook(const std::string& itemKey, const std::string& attachmentKey) override;
    void setAllHighlightsOffline(bool) override;
    void removeDownloads() override;
    void setIntervalMinutes(int) override;
    bool poll(SyncEvent& out) override;
    bool busy() const override { return busy_; }
    void downloadKept() override;
    std::optional<KeptStatus> keptStatus(const std::string& collectionKey) const override;
    void removeLibrary(int64_t lib, bool evenUnsent) override;

private:
    struct Command {
        // KeepDownloads: one file of the kept collections, then itself again (flag: a fresh start).
        enum class Kind { Sync, SyncWhenOnline, OpenBook, SetAllOffline, RemoveDownloads, Push, KeepDownloads, RemoveLibrary } kind;
        std::string itemKey, attachmentKey;
        bool flag = false;
        int64_t lib = 0;  // RemoveLibrary's (flag: even with unsent changes)
        uint64_t request = 0;  // syncNowTracked(), removeLibrary(): told back on SyncStarted/SyncFinished
    };
    struct Worker;

    uint64_t enqueue(Command);  // the request id of the command that will run it (0: none)
    void run();
    void emit(SyncEvent);
    bool sleepFor(int seconds);  // false when stopping
    bool sleepForMs(int ms);
    // PushStarted/PushFinished around SyncEngine::push, if anything is unsent; first the refusal of
    // plugin changes no group can take. Returns the merged report (groups' errors scoped to them);
    // `passed` gains the groups pushed to.
    SyncReport pushChanges(Worker&, std::vector<int64_t>* passed = nullptr);
    // The file into filesDir (DownloadProgress on the way) and marked Local; false with *err if not.
    // onTotal: once, when the size is known.
    bool download(Worker&, const std::string& itemKey, const AttachmentRecord&, UserError* err,
                  const std::function<void(int64_t total)>& onTotal = nullptr);
    bool keepStep(Worker&, bool fresh);  // true: more to download
    void setKept(std::map<std::string, KeptStatus>);  // KeptChanged if it changed

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
    std::map<std::string, KeptStatus> kept_;  // guarded by mutex_: collections catching up
    // Guarded by mutex_: a kept file is downloading; openBook() cancels it (yield), as it goes first.
    bool keptRunning_ = false, keptYield_ = false;
    uint64_t nextRequest_ = 0;  // guarded by mutex_
};

}  // namespace zk
