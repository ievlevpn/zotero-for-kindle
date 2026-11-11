#pragma once
// Library tab: the items, Kindle-like. "Library", All | Downloaded under it, Filter and Sort
// buttons; a quiet line naming the filter applied (with Clear) and the count; the list a page at
// a time, with section headings by Title, Author, Year or Date added. The footer ("12 / 359")
// opens the jump sheet. The view is remembered on this Kindle (library_sheets.h).

#include <functional>
#include <optional>

#include "screens/item_list.h"
#include "screens/library_sheets.h"
#include "screens/screen.h"

namespace zk {

class LibraryScreen final : public Screen {
public:
    explicit LibraryScreen(std::function<void(const ItemRow&)> openItem, StatusFn status = {});

    void draw(Canvas&) override;
    void onGesture(const Gesture&, Nav&) override;
    void onSyncEvent(const SyncEvent&, Nav&) override;
    void onShow(Nav&) override;

    const LibraryView& view() const { return view_; }
    void setView(const LibraryView&, Nav&);  // applies it (page 1) and remembers it
    ListQuery query() const { return view_.query(); }
    std::string filterLine() const;
    ItemList& list() { return list_; }
    const FilterSheet* filterSheet() const { return filter_ ? &*filter_ : nullptr; }
    const SortSheet* sortSheet() const { return sort_ ? &*sort_ : nullptr; }
    const JumpSheet* jumpSheet() const { return jump_ ? &*jump_ : nullptr; }

    static Rect filterButtonRect();
    static Rect sortButtonRect();
    static Rect segmentRect(int index);  // 0 All, 1 Downloaded
    static Rect filterLineRect();
    static Rect clearRect();  // on the filter line, while the filter isn't the default
    static Rect listArea();
    static Rect footerRect();  // the page indicator: opens the jump sheet
    static Rect sheetArea();   // what the sheets cover: everything under the title

private:
    void apply(Nav&);
    void closeSheets();
    void changed(Nav&);  // the view changed: list, filter line and toggle repainted
    void onFilterGesture(const Gesture&, Nav&);
    void onSortGesture(const Gesture&, Nav&);
    void onJumpGesture(const Gesture&, Nav&);
    void openJump(Nav&);

    std::function<void(const ItemRow&)> openItem_;
    ItemList list_;
    LibraryView view_;
    bool loaded_ = false;
    std::optional<FilterSheet> filter_;
    std::optional<SortSheet> sort_;
    std::optional<JumpSheet> jump_;
};

}  // namespace zk
