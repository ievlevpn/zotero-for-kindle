#pragma once
// The Library tab's view (All | Downloaded, the filter, the sort; remembered on this Kindle) and
// the sheets that change it: Filter (Type · Read · Tag, Reset, Done), Sort, and the jump sheet the
// page footer opens (the initials, years or months there are; by Recent, First page and Go to page).
// Each sheet draws over the bottom of an area and takes the gestures while it is open; a tap
// outside it closes it.

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

    bool filtered() const;           // the filter (Type, Read, Tag) differs from the default
    LibraryView cleared() const;     // the filter back to the default; All | Downloaded and the sort stay
    ListQuery query() const;
    bool operator==(const LibraryView&) const = default;
};

// Kept in the library's meta table (this Kindle's), as JSON; a missing or broken one is the default.
LibraryView loadLibraryView(Library&);
void saveLibraryView(Library&, const LibraryView&);

// "2,153 items", or what is applied first: "PDF · Unread · tag: to-read · 412 items".
std::string filterLine(const LibraryView&, int64_t count);
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
    explicit FilterSheet(LibraryView draft) : draft_(std::move(draft)) {}
    void draw(cairo_t*, Rect area) const;
    Action onGesture(const Gesture&, Rect area);

    const LibraryView& draft() const { return draft_; }
    void setTag(const std::string& tag) { draft_.tag = tag; }

    Rect sheetRect(Rect area) const;
    std::vector<Rect> typeRects(Rect area) const;  // Any · With a file · PDF · EPUB
    std::vector<Rect> readRects(Rect area) const;  // Any · Unread · Read
    std::vector<Rect> tagRects(Rect area) const;   // None · the tag (or "Choose a tag…")
    Rect resetRect(Rect area) const;
    Rect doneRect(Rect area) const;

private:
    Rect chipsArea(Rect area, int group) const;
    std::vector<std::string> tagLabels() const;
    LibraryView draft_;
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

}  // namespace zk
