#pragma once
// Home (mockup C, "Hybrid"): title with sync status, search field, Continue reading card,
// Recent items; an error banner above the tab bar when the last sync failed.

#include <functional>
#include <optional>
#include <vector>

#include "screens/item_sheet.h"
#include "screens/screen.h"
#include "screens/sync_status.h"

namespace zk {

struct HomeActions {
    std::function<void()> openSearch;
    std::function<void()> openSettings;
    std::function<void(const ItemRow&)> openItem;
    StatusFn status;  // download progress/errors for the Recent rows
    // "Version X is available" (UpdateNotice): the version, asked on show and every tick.
    std::function<std::optional<std::string>()> updateNotice;
    std::function<void()> openUpdate;
    std::function<void()> dismissUpdate;  // ×
    // Safe mode, the automatic case: plugins were turned off because the app stopped. True until
    // the user opens Settings → Plugins (here or by hand). A tap on the notice calls openPlugins.
    std::function<bool()> safeModeNotice;
    std::function<void()> openPlugins;
};

class HomeScreen final : public Screen {
public:
    HomeScreen(SyncStatus&, HomeActions, std::function<int64_t()> wallMs);

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;

    static Rect syncIndicatorRect();
    static Rect settingsRect();
    static Rect closeRect();  // "×": close Zotero, back to the Kindle
    static Rect searchFieldRect();
    static Rect cardRect();
    std::vector<Rect> recentRowRects() const;
    // The safe-mode notice: in the error banner's place when there's no error, before the update banner.
    std::optional<std::string> safeModeBannerText() const;
    Rect safeModeBannerRect() const;
    // The update banner: in the error banner's place when there's no error (and no safe-mode notice).
    std::optional<std::string> updateBannerText() const;
    Rect updateBannerRect() const;
    Rect updateDismissRect() const;
    const std::vector<ItemRow>& recent() const { return recent_; }
    const ItemSheet* sheet() const { return sheet_ ? &*sheet_ : nullptr; }

private:
    void reload(Library&);
    Rect bannerRect() const;
    bool bannerShown() const {
        return status_.hasError() || safeModeBannerText().has_value() || updateBannerText().has_value();
    }
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
    std::optional<ItemSheet> sheet_;  // long-press on the card or a Recent row
    Rect sheetArea() const;
    int64_t total_ = 0;
    bool safe_ = false;                  // the safe-mode notice is wanted
    std::optional<std::string> update_;  // the version the banner names
};

}  // namespace zk
