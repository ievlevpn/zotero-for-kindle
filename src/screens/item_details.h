#pragma once
// About this item (More… on the item sheet): every author and editor, type, date, publication,
// collections, tags, each file with its own Remove from Kindle (and Open, when there are two or
// more EPUBs and PDFs), the plugins' item actions (a Plugins section: title and plugin name;
// "Working…" while one runs, then its result pop-up), and the whole abstract.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "screens/plugin_entries.h"
#include "screens/plugin_result.h"
#include "screens/screen.h"
#include "store/library.h"

namespace zk {

class ItemDetailsScreen final : public Screen {
public:
    // `open` opens the item (as a tap on its row does); may be null.
    ItemDetailsScreen(ItemRow, std::function<void(const ItemRow&)> open);
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onShow(Nav&) override;
    void onTick(Nav&) override;
    void onClose(Nav&) override;
    bool showsTabBar() const override { return false; }

    static Rect openRect();
    std::vector<Rect> removeRects() const;  // one per file on the Kindle, in files order (else empty)
    std::vector<Rect> fileOpenRects() const;  // one per readable file if more than one (and `open`), in files order
    const ItemDetails& details() const { return details_; }
    const std::vector<PluginAction>& actions() const { return actions_; }
    std::vector<Rect> actionRects() const;  // the Plugins section's rows (laid out by draw)
    const PluginResult* result() const { return result_.get(); }

private:
    ItemRow row_;
    std::function<void(const ItemRow&)> open_;
    ItemDetails details_;
    std::string confirming_;  // attachment key whose Remove was tapped once
    mutable std::vector<std::pair<std::string, Rect>> removeTargets_, openTargets_;  // laid out by draw
    std::vector<PluginAction> actions_;
    mutable std::vector<Rect> actionRects_;  // laid out by draw
    std::unique_ptr<PluginResult> result_;
    int running_ = -1;  // the action whose run result_ is
    uint64_t entriesVersion_ = 0;
};

}  // namespace zk
