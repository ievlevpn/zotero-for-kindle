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
#include "ui/widgets.h"

namespace zk {

// A run of rows under one heading ("A", "2025", "March 2025"); key as SectionCount's.
struct ListSection {
    std::string key, label;
    int64_t count = 0;
};

// A heading takes the space of half a row.
constexpr int kHeadingH = kRowH / 2;

class ItemList {
public:
    // The page source: (offset, limit) -> rows, and the total count; with sections, the headings.
    using Fetch = std::function<std::vector<ItemRow>(Library&, int64_t offset, int limit)>;
    using Count = std::function<int64_t(Library&)>;
    using Sections = std::function<std::vector<ListSection>(Library&)>;

    ItemList() : pager_(0, kRowsPerPageDefault) {}
    static ItemList forQuery(ListQuery q);
    // Back to page 1. With sections, their counts are the total (Count isn't asked) and the
    // pages hold fewer rows where headings take room.
    void setSource(Fetch, Count, Library&, Sections = {});
    void refresh(Library&);  // same page, new data
    void setRowsPerPage(int n) { perPage_ = n; }
    int rowsPerPage() const { return perPage_; }
    bool goToPage(int page, Library&);  // false if there is no such page
    const std::vector<ListSection>& sections() const { return sections_; }
    int sectionPage(size_t section) const;  // the page with the section's first row
    // Where each page starts, for sections of these sizes, `rowsPerPage` rows to a page without
    // headings: a heading takes half a row, and is never the last thing on a page.
    static std::vector<int64_t> pageStarts(const std::vector<ListSection>&, int rowsPerPage);

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
    std::vector<std::pair<std::string, Rect>> headings(Rect area) const;  // on this page
    const Pager& pager() const { return pager_; }

    static int rowsFitting(Rect area);

private:
    static constexpr int kRowsPerPageDefault = 6;
    void load(Library&);
    void count(Library&);  // the total, and the pages with sections
    bool startsSection(int64_t row) const;
    const ListSection* sectionAt(int64_t row) const;  // the section `row` starts, if it starts one

    Fetch fetch_;
    Count count_;
    Sections sectionsOf_;
    std::vector<ListSection> sections_;
    std::vector<int64_t> sectionStarts_;  // each section's first row
    Pager pager_;
    int perPage_ = kRowsPerPageDefault;
    std::vector<ItemRow> rows_;
    StatusFn statusFn_;
    std::optional<ItemSheet> sheet_;
};

}  // namespace zk
