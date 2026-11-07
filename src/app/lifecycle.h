#pragma once
// Background/foreground and storage watch (spec §7.8): while the Kindle sleeps no periodic sync
// runs; on wake the screen gets a full refresh and, with Wi-Fi up, a sync if the last one is
// older than 2 minutes (or failed) — the worker waits for Wi-Fi. If the app's storage goes away (USB storage mode), the app quits.

#include <functional>

#include "app/app.h"
#include "screens/sync_status.h"

namespace zk {

class Lifecycle {
public:
    static constexpr int64_t kResyncAfterMs = 2 * 60'000;
    static constexpr int64_t kStorageCheckMs = 5'000;

    Lifecycle(SyncControl&, SyncStatus&, std::function<int()> intervalMinutes, std::function<bool()> storageOk);
    void attach(App&);  // installs the hooks; the App must outlive nothing beyond this object

    void onBackground();
    void onForeground(int64_t nowMs);
    bool watch();  // every few seconds: is the app's storage still there?

private:
    SyncControl& sync_;
    SyncStatus& status_;
    std::function<int()> interval_;
    std::function<bool()> storage_;
};

}  // namespace zk
