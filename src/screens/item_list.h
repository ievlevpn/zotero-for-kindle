#pragma once
// A paged list of items (Library, a collection, search results): one page of rows at a time,
// swipe up/down to turn pages, per-row status text (download progress or an error).

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "screens/item_sheet.h"
#include "screens/screen.h"
#include "ui/pager.h"

namespace zk {

class ItemList {
public:
    // The page source: (offset, limit) -> rows, and the total count.
    using Fetch = std::function<std::vector<ItemRow>(Library&, int64_t offset, int limit)>;
    using Count = std::function<int64_t(Library&)>;

    ItemList() : pager_(0, kRowsPerPageDefault) {}
    static ItemList forQuery(ListQuery q);
    void setSource(Fetch, Count, Library&);  // back to page 1
    void refresh(Library&);                  // same page, new data
    void setRowsPerPage(int n) { perPage_ = n; }

    void draw(cairo_t*, Rect area) const;
    // Swipes turn pages (damaging `area`); taps on a row call `open`; a long-press opens the item
    // sheet over `area` (Open / Remove from Kindle / More…), which then takes the gestures. True if handled.
    bool onGesture(const Gesture&, Rect area, Nav&, const std::function<void(const ItemRow&)>& open);

    void setStatusFn(StatusFn f) { statusFn_ = std::move(f); }
    std::string statusFor(const std::string& attachmentKey) const;
    // A download started/progressed/finished for a row on this page: repaint that row (Fast).
    void onDownloadEvent(const SyncEvent&, Rect area, Nav&);
    int indexOfAttachment(const std::string& attachmentKey) const;  // on this page; -1 if not shown

    const std::vector<ItemRow>& rows() const { return rows_; }
    const ItemSheet* sheet() const { return sheet_ ? &*sheet_ : nullptr; }
    std::vector<Rect> rowRects(Rect area) const;
    const Pager& pager() const { return pager_; }

    static int rowsFitting(Rect area);

private:
    static constexpr int kRowsPerPageDefault = 6;
    void load(Library&);

    Fetch fetch_;
    Count count_;
    Pager pager_;
    int perPage_ = kRowsPerPageDefault;
    std::vector<ItemRow> rows_;
    StatusFn statusFn_;
    std::optional<ItemSheet> sheet_;
};

}  // namespace zk
