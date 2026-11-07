#pragma once
// Settings (spec §7.7): account, WebDAV, sync interval, "Keep all highlights offline",
// downloaded files, about.

#include <functional>
#include <optional>
#include <string>

#include "screens/screen.h"
#include "sync/config.h"
#include "update/updater.h"

namespace zk {

class SettingsScreen final : public Screen {
public:
    // `updater` (may be null): the About row's Check for updates; `openUpdate` pushes the update
    // screen when the check finds a newer release. `pendingUpdate`: a newer version the weekly
    // check found (UpdateNotice), for the About row.
    SettingsScreen(Config, std::function<void(Nav&)> changeAccount, Updater* updater = nullptr,
                   std::function<void(Nav&)> openUpdate = nullptr,
                   std::function<std::optional<std::string>()> pendingUpdate = nullptr);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    bool showsTabBar() const override { return false; }

    Rect intervalRect(int i) const;  // Off, 5, 10, 30 min
    Rect toggleRect() const;
    Rect removeRect() const;
    Rect accountRect() const;
    Rect flashMinusRect() const;  // "Full refresh every N pages" (reader), 1–12
    Rect flashPlusRect() const;
    Rect updateRect() const;
    Rect aboutTextRect() const;  // the About row's second line (two lines at most)
    std::string updateLine() const;  // the About row's second line: version, or the last check's result

private:
    void load(Library&);
    Config config_;
    std::function<void(Nav&)> changeAccount_;
    Updater* updater_ = nullptr;
    std::function<void(Nav&)> openUpdate_;
    std::function<std::optional<std::string>()> pendingUpdate_;
    bool checking_ = false;
    std::string updateResult_;
    int interval_ = 10;
    int flashEvery_ = 6;
    bool allOffline_ = false;
    bool confirmRemove_ = false;
    int64_t downloadedBytes_ = 0;
};

}  // namespace zk
