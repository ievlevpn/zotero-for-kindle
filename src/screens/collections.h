#pragma once
// Collections tab: the top-level collections; a collection shows its subcollections first,
// then its items, one page at a time.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

// The long press on a collection: its name, Pin to top (or Unpin), and Keep downloaded (or Stop
// keeping downloaded), above the tab bar.
class PinSheet {
public:
    PinSheet(CollectionEntry entry, bool pinned) : entry_(std::move(entry)), pinned_(pinned) {}
    static Rect rect();
    static Rect buttonRect();
    static Rect keepButtonRect();
    std::string label() const { return pinned_ ? "Unpin" : "Pin to top"; }
    std::string keepLabel() const { return entry_.kept ? "Stop keeping downloaded" : "Keep downloaded"; }
    void draw(cairo_t*) const;
    // A tap closes the sheet (the caller's part); one on a button pins or unpins, or keeps the
    // collection downloaded (starting the downloads) or stops doing so (deleting nothing).
    void tap(Point, Nav&) const;

private:
    CollectionEntry entry_;
    bool pinned_;
};

// A collection row's note: how its kept downloads are going ("" when there's nothing to say).
std::string keptNote(const std::optional<KeptStatus>&);

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
    std::optional<std::string> keepSheetLabel() const { return sheet_ ? std::optional(sheet_->keepLabel()) : std::nullopt; }
    static Rect pinButtonRect() { return PinSheet::buttonRect(); }
    static Rect keepButtonRect() { return PinSheet::keepButtonRect(); }
    std::string rowNote(size_t i) const;  // the second line of this page's row i

private:
    void load(Library&);
    void loadKept(Nav&);
    void showPage();
    std::optional<PinSheet> sheet_;
    std::map<std::string, KeptStatus> kept_;  // kept collections catching up (as the sync last said)
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
    static Rect searchRect();  // the header's search icon: search in this collection
    Rect lineRect(size_t i) const { return lineRects().at(i); }
    std::optional<std::string> pinSheetLabel() const { return sheet_ ? std::optional(sheet_->label()) : std::nullopt; }
    std::optional<std::string> keepSheetLabel() const { return sheet_ ? std::optional(sheet_->keepLabel()) : std::nullopt; }
    static Rect pinButtonRect() { return PinSheet::buttonRect(); }
    static Rect keepButtonRect() { return PinSheet::keepButtonRect(); }

private:
    void load(Library&);
    void loadKept(Nav&);
    std::vector<Rect> lineRects() const;
    std::optional<PinSheet> sheet_;
    std::map<std::string, KeptStatus> kept_;

    CollectionEntry entry_;
    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    std::vector<CollectionEntry> subs_;
    std::vector<Line> page_;
    Pager pager_{0, 1};
};

}  // namespace zk
