#pragma once
// About this item (More… on the item sheet): every author and editor, type, date, publication,
// collections, tags, each file with its own Remove from Kindle, and the whole abstract.

#include <functional>
#include <string>
#include <vector>

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
    bool showsTabBar() const override { return false; }

    static Rect openRect();
    std::vector<Rect> removeRects() const;  // one per file on the Kindle, in files order (else empty)
    const ItemDetails& details() const { return details_; }

private:
    ItemRow row_;
    std::function<void(const ItemRow&)> open_;
    ItemDetails details_;
    std::string confirming_;  // attachment key whose Remove was tapped once
    mutable std::vector<std::pair<std::string, Rect>> removeTargets_;  // laid out by draw
};

}  // namespace zk
