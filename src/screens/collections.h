#pragma once
// Collections tab: the pinned collections, then the top-level ones by name; a collection shows its
// subcollections first, then its items by title, one page at a time. The page pill ("2 / 5 ›")
// opens the jump sheet: the initials of the names (and of the items' titles), as the Library's.
// With group libraries, a "Group libraries" heading follows My Library's collections, then a line
// per group shown (switched on, still there): tapping one opens its own list, its top-level
// collections, then all its items.

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "screens/library_sheets.h"
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
    // The group libraries (all of them, by name), and those on this page with their lines.
    const std::vector<LibraryInfo>& groups() const { return groups_; }
    std::vector<std::pair<LibraryInfo, Rect>> groupRows() const;
    std::optional<Rect> groupHeadingRect() const;  // "Group libraries", when on this page
    static Rect searchRect();  // the header's search icon
    std::optional<std::string> pinSheetLabel() const { return sheet_ ? std::optional(sheet_->label()) : std::nullopt; }
    std::optional<std::string> keepSheetLabel() const { return sheet_ ? std::optional(sheet_->keepLabel()) : std::nullopt; }
    static Rect pinButtonRect() { return PinSheet::buttonRect(); }
    static Rect keepButtonRect() { return PinSheet::keepButtonRect(); }
    std::string rowNote(size_t i) const;  // the second line of this page's row i
    const Pager& pager() const { return pager_; }
    const JumpSheet* jumpSheet() const { return jump_ ? &*jump_ : nullptr; }
    static Rect footerRect();  // the page pill: opens the jump sheet
    static Rect sheetArea();   // what the jump sheet covers

private:
    void load(Library&);
    void loadKept(Nav&);
    void showPage();
    std::optional<PinSheet> sheet_;
    std::optional<JumpSheet> jump_;
    std::vector<ListSection> sections_;  // the initials of the collections after the pinned ones
    std::vector<int64_t> starts_;        // each section's first entry in all_
    std::map<std::string, KeptStatus> kept_;  // kept collections catching up (as the sync last said)
    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    std::vector<CollectionEntry> all_, entries_;
    std::vector<LibraryInfo> groups_;  // lines after all_: the heading, then one per group
    int64_t lines() const { return static_cast<int64_t>(all_.size() + (groups_.empty() ? 0 : 1 + groups_.size())); }
    Rect lineRect(int64_t line) const;  // a line of this page (an index over all of them)
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
    // A group library's own list: its top-level collections, then all its items.
    CollectionScreen(const LibraryInfo& group, std::function<void(const ItemRow&)> openItem, StatusFn status = {});
    int64_t group() const { return group_; }  // 0: a collection's list
    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const std::vector<Line>& page() const { return page_; }
    int64_t total() const { return pager_.total(); }
    static Rect backRect();
    static Rect searchRect();  // the header's search icon: search in this collection
    Rect lineRect(size_t i) const { return lineRects().at(i); }
    const Pager& pager() const { return pager_; }
    const JumpSheet* jumpSheet() const { return jump_ ? &*jump_ : nullptr; }
    static Rect footerRect() { return CollectionsScreen::footerRect(); }
    static Rect sheetArea() { return CollectionsScreen::sheetArea(); }
    std::optional<std::string> pinSheetLabel() const { return sheet_ ? std::optional(sheet_->label()) : std::nullopt; }
    std::optional<std::string> keepSheetLabel() const { return sheet_ ? std::optional(sheet_->keepLabel()) : std::nullopt; }
    static Rect pinButtonRect() { return PinSheet::buttonRect(); }
    static Rect keepButtonRect() { return PinSheet::keepButtonRect(); }

private:
    void load(Library&);
    void loadKept(Nav&);
    std::vector<Rect> lineRects() const;
    std::vector<JumpGroup> jumpIndex() const;
    std::optional<PinSheet> sheet_;
    std::optional<JumpSheet> jump_;
    std::vector<ListSection> subSections_, itemSections_;
    std::vector<int64_t> starts_;  // each section's first line: the subcollections', then the items'
    std::map<std::string, KeptStatus> kept_;

    CollectionEntry entry_;
    int64_t group_ = 0;  // a group library's list (entry_ only carries its name)
    std::function<void(const ItemRow&)> openItem_;
    StatusFn status_;
    std::vector<CollectionEntry> subs_;
    std::vector<Line> page_;
    Pager pager_{0, 1};
};

}  // namespace zk
