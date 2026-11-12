#include "update/updater.h"

#include "core/log.h"
#include "core/wake.h"

#include <filesystem>

namespace zk {

using Phase = Updater::Status::Phase;

Updater::Updater(UpdaterConfig c) : config_(std::move(c)) {}

Updater::~Updater() {
    *cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool Updater::busy() const {
    const Phase p = status_.phase;
    return p == Phase::Checking || p == Phase::Downloading || p == Phase::Installing;
}

void Updater::set(const std::function<void(Status&)>& change) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        change(status_);
        ++status_.serial;
    }
    wakeUi();  // Settings, the update screen and Home's notice look at it on the next step
}

void Updater::start(Phase phase, std::function<void()> work) {
    if (worker_.joinable()) worker_.join();  // the previous operation has ended (not busy)
    set([&](Status& s) {
        s.phase = phase;
        s.error = {};
        s.done = s.total = 0;
    });
    worker_ = std::thread(std::move(work));
}

void Updater::check() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy() || status_.phase == Phase::Downloaded || status_.phase == Phase::ReadyToRestart ||
            status_.mustRestart)
            return;
    }
    start(Phase::Checking, [this] {
        auto http = config_.makeHttp();
        const CheckResult r = checkLatest(*http, config_.feedUrl, config_.current, config_.probe);
        log(LogLevel::Info, "update: checked %s: %s", config_.feedUrl.c_str(),
            r.kind == CheckResult::Kind::Available        ? ("available " + r.release.version.str()).c_str()
            : r.kind == CheckResult::Kind::UpToDate       ? "up to date"
            : r.kind == CheckResult::Kind::NotInstallable ? "newer, not installable"
                                                          : r.error.details.c_str());
        set([&](Status& s) {
            s.release = r.release;
            s.error = r.error;
            s.phase = r.kind == CheckResult::Kind::Available        ? Phase::Available
                      : r.kind == CheckResult::Kind::UpToDate       ? Phase::UpToDate
                      : r.kind == CheckResult::Kind::NotInstallable ? Phase::NotInstallable
                                                                    : Phase::Failed;
        });
    });
}

void Updater::download() {
    ReleaseInfo release;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool retry = status_.phase == Phase::Failed && !status_.mustRestart && status_.release.zipSize > 0;
        if (!canInstall() || status_.mustRestart || !(status_.phase == Phase::Available || retry)) return;
        release = status_.release;
    }
    *cancel_ = false;
    start(Phase::Downloading, [this, release] {
        auto http = config_.makeHttp();
        const std::string work = config_.paths.updateDir();
        UserError err;
        std::string zip;
        DownloadHooks hooks = config_.hooks;
        std::error_code sizeError;
        for (const std::string& f : {config_.paths.dbPath, config_.paths.dbPath + "-wal"})  // backed up while installing
            if (auto n = std::filesystem::file_size(f, sizeError); !sizeError) hooks.extraBytes += static_cast<int64_t>(n);
        bool ok = downloadRelease(
            *http, release, config_.publicKey, work,
            [this](int64_t done, int64_t total) {
                set([&](Status& s) {
                    s.done = done;
                    s.total = total;
                });
            },
            cancel_, &zip, &err, hooks);
        // Cancel counts until the download is ready, checks and unpacking included.
        auto cancelled = [&] {
            if (!*cancel_) return false;
            err = {ErrorKind::Cancelled, "Cancelled.", "cancelled"};
            return true;
        };
        if (ok && cancelled()) ok = false;
        if (ok) ok = unpackRelease(zip, work + "/new", &err);
        std::error_code ec;
        std::filesystem::remove(work + "/zotero-for-kindle.zip", ec);  // unpacked or not, the install doesn't use it
        if (ok && cancelled()) ok = false;
        if (!ok) std::filesystem::remove_all(work + "/new", ec);
        set([&](Status& s) {
            if (ok) {
                s.phase = Phase::Downloaded;
            } else if (err.kind == ErrorKind::Cancelled) {
                s.phase = Phase::Available;
            } else {
                s.phase = Phase::Failed;
                s.error = err;
            }
        });
    });
}

void Updater::install() {
    ReleaseInfo release;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.phase != Phase::Downloaded || status_.mustRestart) return;
        release = status_.release;
    }
    start(Phase::Installing, [this, release] {
        UserError err;
        const std::string newDir = config_.paths.updateDir() + "/new";
        const bool ok = installRelease(config_.paths, newDir, config_.current, release.version, nullptr, &err);
        // Past "installing", only the launcher can put things right: restart into it.
        const bool restart = !ok && std::filesystem::exists(config_.paths.updateDir() + "/state");
        set([&](Status& s) {
            s.phase = ok ? Phase::ReadyToRestart : Phase::Failed;
            s.error = err;
            s.mustRestart = restart;
        });
    });
}

void Updater::cancel() { *cancel_ = true; }

void Updater::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy() || status_.phase == Phase::Downloaded || status_.phase == Phase::ReadyToRestart || status_.mustRestart)
        return;
    status_.phase = Phase::Idle;
    status_.error = {};
    ++status_.serial;
}

Updater::Status Updater::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Updater::wait() {
    if (worker_.joinable()) worker_.join();
}

}  // namespace zk
