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
    app.setWatchdog(
        [this] {
            if (storage_()) return true;
            log(LogLevel::Warn, "app storage is gone (USB storage mode?): exiting");
            return false;
        },
        kStorageCheckMs);
}

void Lifecycle::onBackground() { sync_.setIntervalMinutes(0); }

void Lifecycle::onForeground(int64_t nowMs) {
    sync_.setIntervalMinutes(interval_());
    if (wifi_() && !status_.syncing() && status_.msSinceSync(nowMs) > kResyncAfterMs) sync_.syncNow();
}

}  // namespace zk
