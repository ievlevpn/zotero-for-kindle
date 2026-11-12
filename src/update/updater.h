#pragma once
// The app's side of updating: one operation at a time on a worker thread, its progress read
// by the UI through status(). Nothing happens unless the user asks (Settings → Check for updates).
//   check()    Checking → UpToDate | Available | NotInstallable | Failed
//   download() Available → Downloading → Downloaded (verified and unpacked) | Failed | back to
//              Available when cancelled
//   install()  Downloaded → Installing → ReadyToRestart (exit 75) | Failed. Call it with sync
//              stopped. A failure after the swap began sets mustRestart: the launcher rolls back.

#include "net/http.h"
#include "update/download.h"
#include "update/install.h"
#include "update/release_check.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace zk {

struct UpdaterConfig {
    std::function<std::unique_ptr<Http>()> makeHttp;
    std::string feedUrl = kDefaultFeed;
    std::array<uint8_t, 32> publicKey{};
    InstallPaths paths;  // appDir "" = this build can't install (the desktop)
    Version current;
    DownloadHooks hooks;
    std::function<NetState()> probe = probeNetState;
};

class Updater {
public:
    struct Status {
        enum class Phase { Idle, Checking, UpToDate, Available, NotInstallable, Downloading, Downloaded, Installing,
                           ReadyToRestart, Failed };
        Phase phase = Phase::Idle;
        ReleaseInfo release;
        int64_t done = 0, total = 0;  // download bytes
        UserError error;
        bool mustRestart = false;
        int serial = 0;  // changes with every status change (for redraws)
    };

    explicit Updater(UpdaterConfig);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    void check();
    void download();
    void install();
    void cancel();  // a download in progress
    void reset();   // back to Idle unless busy
    Status status() const;
    bool canInstall() const { return !config_.paths.appDir.empty(); }
    Version current() const { return config_.current; }
    void wait();  // until the current operation ends (tests)

private:
    bool busy() const;
    void start(Status::Phase phase, std::function<void()> work);
    void set(const std::function<void(Status&)>& change);

    UpdaterConfig config_;
    mutable std::mutex mutex_;
    Status status_;
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace zk
