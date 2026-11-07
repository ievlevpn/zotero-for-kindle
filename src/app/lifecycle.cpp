#include "app/lifecycle.h"

#include "core/log.h"

namespace zk {

Lifecycle::Lifecycle(SyncControl& sync, SyncStatus& status, std::function<int()> interval, std::function<bool()> wifi,
                     std::function<bool()> storage)
    : sync_(sync), status_(status), interval_(std::move(interval)), wifi_(std::move(wifi)), storage_(std::move(storage)) {}

void Lifecycle::attach(App& app) {
    app.setLifecycleHook([this, &app](bool foreground) {
        if (foreground) onForeground(app.nowMs());
        else onBackground();
    });
    app.setWatchdog([this] { return watch(); }, kStorageCheckMs);
}

bool Lifecycle::watch() {
    if (!storage_()) {
        log(LogLevel::Warn, "app storage is gone (USB storage mode?): exiting");
        return false;
    }
    if (syncWhenOnline_ && wifi_()) {
        syncWhenOnline_ = false;
        sync_.syncNow();
    }
    return true;
}

void Lifecycle::onBackground() {
    sync_.setIntervalMinutes(0);
    syncWhenOnline_ = false;
}

void Lifecycle::onForeground(int64_t nowMs) {
    sync_.setIntervalMinutes(interval_());
    if (status_.syncing() || (!status_.hasError() && status_.msSinceSync(nowMs) <= kResyncAfterMs)) return;
    if (wifi_()) sync_.syncNow();
    else syncWhenOnline_ = true;
}

}  // namespace zk
