#pragma once
// Collections tab: the top-level collections; a collection shows its subcollections first,
// then its items, one page at a time.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

// The long press on a collection: its name, and Pin to top (or Unpin), above the tab bar.
class PinSheet {
public:
    PinSheet(CollectionEntry entry, bool pinned) : entry_(std::move(entry)), pinned_(pinned) {}
    static Rect rect();
    static Rect buttonRect();
    std::string label() const { return pinned_ ? "Unpin" : "Pin to top"; }
    void draw(cairo_t*) const;
    // A tap: true when it closes the sheet (always); on the button, pins or unpins.
    void tap(Point, Library&) const;

private:
    CollectionEntry entry_;
    bool pinned_;
};

class CollectionsScreen final : public Screen {
public:
    explicit CollectionsScreen(std::function<void(const ItemRow&)> openItem, StatusFn status = {});
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const std::vector<CollectionEntry>& entries() const { return entries_; }  // this page
    std::vector<Rect> entryRects() const;
    static Rect searchRect();  // the header's search icon
    std::optional<std::string> pinSheetLabel() const { return sheet_ ? std::optional(sheet_->label()) : std::nullopt; }
    static Rect pinButtonRect() { return PinSheet::buttonRect(); }

private:
    void load(Library&);
    void showPage();
    std::optional<PinSheet> sheet_;
    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    std::vector<CollectionEntry> all_, entries_;
    Pager pager_{0, 1};
};

class CollectionScreen final : public Screen {
public:
    struct Line {
        bool isCollection = false;
        CollectionEntry collection;
        ItemRow item;
    };

    CollectionScreen(CollectionEntry, std::function<void(const ItemRow&)> openItem, StatusFn status = {});
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const std::vector<Line>& page() const { return page_; }
    int64_t total() const { return pager_.total(); }
    static Rect backRect();
    Rect lineRect(size_t i) const { return lineRects().at(i); }
    std::optional<std::string> pinSheetLabel() const { return sheet_ ? std::optional(sheet_->label()) : std::nullopt; }
    static Rect pinButtonRect() { return PinSheet::buttonRect(); }

private:
    void load(Library&);
    std::vector<Rect> lineRects() const;
    std::optional<PinSheet> sheet_;

    CollectionEntry entry_;
    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    std::vector<CollectionEntry> subs_;
    std::vector<Line> page_;
    Pager pager_{0, 1};
};

}  // namespace zk
