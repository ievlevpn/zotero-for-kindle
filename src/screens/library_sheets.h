#pragma once
// The Library tab's view (All | Downloaded, the filter, the sort; remembered on this Kindle) and
// the sheets that change it: Filter (Library when there are groups · Type · Read · Tag, Reset, Done), Sort, and the jump sheet the
// page footer opens (the initials, years or months there are; by Recent, First page and Go to page).
// Each sheet draws over the bottom of an area and takes the gestures while it is open; a tap
// outside it closes it.

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cairo.h>

#include "screens/item_list.h"
#include "screens/screen.h"
#include "store/library.h"

namespace zk {

struct LibraryView {
    bool downloaded = false;  // All | Downloaded
    TypeFilter type = TypeFilter::WithFile;
    ReadFilter read = ReadFilter::Any;
    std::string tag;  // "" none
    SortBy sort = SortBy::Recent;
    int64_t lib = kAllLibraries;  // the Library filter: all, My Library (0) or a group's ID

    bool filtered() const;           // the filter (Library, Type, Read, Tag) differs from the default
    LibraryView cleared() const;     // the filter back to the default; All | Downloaded and the sort stay
    ListQuery query() const;
    bool operator==(const LibraryView&) const = default;
};

// Kept in the library's meta table (this Kindle's), as JSON; a missing or broken one is the default.
LibraryView loadLibraryView(Library&);
void saveLibraryView(Library&, const LibraryView&);

// "2,153 items", or what is applied first: "PDF · Unread · tag: to-read · 412 items"; with a
// library chosen, its name leads ("Lab group · 120 items").
std::string filterLine(const LibraryView&, int64_t count, const std::string& libraryName = "");
std::string withThousands(int64_t);  // 2153 -> "2,153"
const char* sortLabel(SortBy);       // "Recent", "Date added", "Title", "Author", "Year"
// A section's heading: "A", "#", "2025", "No year", "March 2025", "No date".
std::string sectionLabel(SortBy, const std::string& key);

// One cell of the jump sheet: the section it opens (an index into the list's sections).
struct JumpEntry {
    std::string label;
    size_t section = 0;
};
// A row of cells under a label: "A–Z", "А–Я" (and "#" alone), a year's months, or all the years.
struct JumpGroup {
    std::string label;
    std::vector<JumpEntry> entries;
};
// The jump index for the sections of a list in that sort (none by Recent).
std::vector<JumpGroup> jumpGroups(SortBy, const std::vector<ListSection>&);

// Common to the three: fades `area`, draws a sheet `height` high at its bottom, titled.
Rect sheetRectIn(Rect area, int height);

class FilterSheet {
public:
    enum class Action { None, Close, Changed, PickTag, Apply };
    // `libraries`: those shown (Library::libraries(false)); with more than My Library, the sheet
    // has a Library group first: All · My Library · each group.
    explicit FilterSheet(LibraryView draft, std::vector<LibraryInfo> libraries = {})
        : draft_(std::move(draft)), libraries_(std::move(libraries)) {
        if (libraries_.size() < 2) libraries_.clear();
    }
    void draw(cairo_t*, Rect area) const;
    Action onGesture(const Gesture&, Rect area);

    const LibraryView& draft() const { return draft_; }
    void setTag(const std::string& tag) { draft_.tag = tag; }

    Rect sheetRect(Rect area) const;
    bool hasLibraries() const { return !libraries_.empty(); }
    std::vector<std::string> libraryLabels() const;   // All · My Library · the groups (names shortened)
    std::vector<Rect> libraryRects(Rect area) const;  // the same (on as many rows as they need); none without groups
    std::vector<Rect> typeRects(Rect area) const;  // Any · With a file · PDF · EPUB
    std::vector<Rect> readRects(Rect area) const;  // Any · Unread · Read
    std::vector<Rect> tagRects(Rect area) const;   // None · the tag (or "Choose a tag…")
    Rect resetRect(Rect area) const;
    Rect doneRect(Rect area) const;

private:
    Rect chipsArea(Rect area, int group) const;  // 0 Type, 1 Read, 2 Tag, -1 Library (its first row)
    // The Library chips' rows: the labels' indices on each (as many as fit a row; as many rows as the
    // area has room for, 3 at most).
    std::vector<std::vector<size_t>> libraryRows(Rect area) const;
    int libraryExtraH(Rect area) const;  // what rows after the first add to the sheet
    int groups() const { return hasLibraries() ? 4 : 3; }
    std::vector<std::string> tagLabels() const;
    LibraryView draft_;
    std::vector<LibraryInfo> libraries_;
};

class SortSheet {
public:
    enum class Action { None, Close, Chosen };
    explicit SortSheet(SortBy current) : current_(current) {}
    void draw(cairo_t*, Rect area) const;
    Action onGesture(const Gesture&, Rect area);
    SortBy chosen() const { return current_; }

    Rect sheetRect(Rect area) const;
    std::vector<Rect> optionRects(Rect area) const;  // Recent · Date added · Title · Author · Year

private:
    SortBy current_;
};

class JumpSheet {
public:
    enum class Action { None, Close, Changed, Jump, FirstPage, GoToPage };
    // No groups: by Recent (First page, and the page field that opens Go to).
    JumpSheet(std::vector<JumpGroup> groups, int page, int pages) : groups_(std::move(groups)), page_(page), pages_(pages) {}
    void draw(cairo_t*, Rect area) const;
    // Jump: target() is the section to open. A swipe turns the sheet's own pages, if it has more.
    Action onGesture(const Gesture&, Rect area);
    size_t target() const { return target_; }

    const std::vector<JumpGroup>& groups() const { return groups_; }
    Rect sheetRect(Rect area) const;
    std::vector<std::pair<std::string, Rect>> cells(Rect area) const;  // on the sheet's page shown
    Rect firstPageRect(Rect area) const;
    Rect pageFieldRect(Rect area) const;
    int sheetPages(Rect area) const;

private:
    struct Placed {
        int sheetPage;
        Rect rect;  // relative to the content's top-left
        std::string label;
        bool isGroupLabel;
        size_t section;
    };
    struct Grid {
        int cellW, cellH, width;
    };
    Grid grid(Rect area) const;
    int cellWidth(const Grid&, const std::string& label) const;
    template <class F> int flow(const Grid&, F&& place) const;
    std::vector<Placed> layout(Rect area) const;
    int contentHeight(const Grid&) const;
    int contentHeight(Rect area) const;

    std::vector<JumpGroup> groups_;
    int page_, pages_;
    int sheetPage_ = 0;
    size_t target_ = 0;
};

// A gesture on an open jump sheet over `area`, for a list now on `page` of `pages`: closes it as
// its action says, and turns the list with go(page) — a section's page from sectionPage(section),
// the first, or one entered on Go to page (pushed). Damages what it closes or changes.
void jumpSheetGesture(std::optional<JumpSheet>& sheet, const Gesture&, Rect area, Nav&, int page, int pages,
                      const std::function<int(size_t section)>& sectionPage, const std::function<void(int page)>& go);

// The jump index of names in listSortKey order (collections): their initials, grouped by script as
// the Library's Title sort. starts: each section's first name (an index into `names`).
std::vector<ListSection> nameSections(const std::vector<std::string>& names, std::vector<int64_t>* starts);

}  // namespace zk
