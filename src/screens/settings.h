#pragma once
// Settings (spec §7.7): account, WebDAV, sync interval, "Keep all highlights offline",
// downloaded files, about.

#include <functional>

#include "screens/screen.h"
#include "sync/config.h"

namespace zk {

class SettingsScreen final : public Screen {
public:
    SettingsScreen(Config, std::function<void(Nav&)> changeAccount);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;
    bool showsTabBar() const override { return false; }

    Rect intervalRect(int i) const;  // Off, 5, 10, 30 min
    Rect toggleRect() const;
    Rect removeRect() const;
    Rect accountRect() const;

private:
    void load(Library&);
    Config config_;
    std::function<void(Nav&)> changeAccount_;
    int interval_ = 10;
    bool allOffline_ = false;
    bool confirmRemove_ = false;
    int64_t downloadedBytes_ = 0;
};

}  // namespace zk
