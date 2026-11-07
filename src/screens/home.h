#pragma once
// Home (mockup C, "Hybrid"): title with sync status, search field, Continue reading card,
// Recent items; an error banner above the tab bar when the last sync failed.

#include <functional>
#include <optional>
#include <vector>

#include "screens/screen.h"
#include "screens/sync_status.h"

namespace zk {

struct HomeActions {
    std::function<void()> openSearch;
    std::function<void()> openSettings;
    std::function<void(const ItemRow&)> openItem;
    StatusFn status;  // download progress/errors for the Recent rows
};

class HomeScreen final : public Screen {
public:
    HomeScreen(SyncStatus&, HomeActions, std::function<int64_t()> wallMs);

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    static Rect syncIndicatorRect();
    static Rect settingsRect();
    static Rect searchFieldRect();
    static Rect cardRect();
    std::vector<Rect> recentRowRects() const;
    const std::vector<ItemRow>& recent() const { return recent_; }

private:
    void reload(Library&);
    Rect bannerRect() const;
    std::string cardMeta() const;

    SyncStatus& status_;
    HomeActions actions_;
    std::function<int64_t()> wallMs_;
    int64_t nowMs_ = 0;  // Nav clock at the last event, for the status line
    std::optional<ItemRow> card_;
    bool cardOpened_ = false;
    int64_t cardOpenedMs_ = 0;
    size_t cardHighlights_ = 0;
    std::vector<ItemRow> recent_;
    int64_t total_ = 0;
};

}  // namespace zk
