#pragma once
// Fit assertions for the Library tab's header, sheets and jump letters, on whatever device the
// metrics are set to (KT4, Scribe): everything inside the window, above the tab bar.

#include <doctest/doctest.h>

#include "screens/library.h"
#include "screens/library_sheets.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <glib.h>

namespace zk::test {

inline bool within(const Rect& r, const Rect& outer) {
    return r.x >= outer.x && r.y >= outer.y && r.x + r.w <= outer.x + outer.w && r.y + r.h <= outer.y + outer.h;
}

inline bool apart(const Rect& a, const Rect& b) {
    return a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
}

// Every jump cell of `sheet`, over all its pages, inside the sheet.
inline size_t checkJumpCells(JumpSheet sheet, Rect area) {
    size_t n = 0;
    const Rect s = sheet.sheetRect(area);
    CHECK(within(s, area));
    for (int p = 0; p < sheet.sheetPages(area); ++p) {
        if (p) sheet.onGesture(Gesture{GestureType::Swipe, {600, 700}, {600, 700}, Direction::Up}, area);
        for (const auto& [label, r] : sheet.cells(area)) {
            CAPTURE(label);
            CHECK(within(r, s));
            ++n;
        }
    }
    return n;
}

inline void checkLibraryNavFits(LibraryScreen& library) {
    const Rect window{0, 0, metrics().width, metrics().height - kTabBarH};
    const int titleW = measureText("Library", {theme::kSans, 17, 700}, window.w, 1).width;
    CHECK(LibraryScreen::filterButtonRect().x > kMargin + titleW);
    for (Rect r : {LibraryScreen::filterButtonRect(), LibraryScreen::sortButtonRect(), LibraryScreen::segmentRect(0),
                   LibraryScreen::segmentRect(1), LibraryScreen::filterLineRect(), LibraryScreen::clearRect(),
                   LibraryScreen::listArea(), LibraryScreen::footerRect(), LibraryScreen::sheetArea()})
        CHECK(within(r, window));
    CHECK(apart(LibraryScreen::filterButtonRect(), LibraryScreen::sortButtonRect()));
    CHECK(apart(LibraryScreen::segmentRect(1), LibraryScreen::filterLineRect()));
    CHECK(LibraryScreen::listArea().y >= LibraryScreen::filterLineRect().y + LibraryScreen::filterLineRect().h);
    CHECK(library.list().rowsPerPage() >= 3);
    for (const auto& r : library.list().rowRects(LibraryScreen::listArea())) CHECK(within(r, LibraryScreen::listArea()));

    const Rect area = LibraryScreen::sheetArea();
    LibraryView v;
    v.tag = "a-rather-long-tag-name-to-see-it-shortened";
    FilterSheet filter(v);
    const Rect fs = filter.sheetRect(area);
    CHECK(within(fs, area));
    CHECK(filter.typeRects(area).size() == 5);  // Any · With a file · PDF · EPUB · Web
    CHECK(filter.readRects(area).size() == 3);
    CHECK(filter.tagRects(area).size() == 2);
    for (const auto& rects : {filter.typeRects(area), filter.readRects(area), filter.tagRects(area)})
        for (const auto& r : rects) CHECK(within(r, fs));
    CHECK(within(filter.resetRect(area), fs));
    CHECK(within(filter.doneRect(area), fs));
    CHECK(apart(filter.resetRect(area), filter.doneRect(area)));

    SortSheet sort(SortBy::Recent);
    CHECK(sort.optionRects(area).size() == 5);
    for (const auto& r : sort.optionRects(area)) CHECK(within(r, sort.sheetRect(area)));

    // Every Latin and Cyrillic initial and "#": one sheet, no paging.
    std::vector<ListSection> letters;
    for (char c = 'A'; c <= 'Z'; ++c) letters.push_back({std::string("1") + c, std::string(1, c), 1});
    for (gunichar c = 0x0410; c <= 0x042F; ++c) {
        char buf[6];
        const std::string s(buf, static_cast<size_t>(g_unichar_to_utf8(c, buf)));
        letters.push_back({"2" + s, s, 1});
    }
    letters.push_back({"3#", "#", 1});
    JumpSheet jump(jumpGroups(SortBy::Title, letters), 0, 400);
    CHECK(jump.sheetPages(area) == 1);
    CHECK(checkJumpCells(jump, area) == letters.size());
    // A century of years: paged if need be, every cell still inside.
    std::vector<ListSection> years;
    for (int y = 2026; y > 1900; --y) years.push_back({std::to_string(y), std::to_string(y), 1});
    years.push_back({"", "No year", 1});
    CHECK(checkJumpCells(JumpSheet(jumpGroups(SortBy::Year, years), 0, 400), area) == years.size());
    JumpSheet pages({}, 11, 359);
    CHECK(within(pages.firstPageRect(area), pages.sheetRect(area)));
    CHECK(within(pages.pageFieldRect(area), pages.sheetRect(area)));
}

}  // namespace zk::test
