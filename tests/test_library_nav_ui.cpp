// The Library tab's navigation on screen: All | Downloaded, the Filter and Sort sheets, the
// filter line, section headings, the jump sheet, and what is remembered.

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/goto_page.h"
#include "screens/library.h"
#include "screens/library_sheets.h"
#include "screens/read_tag.h"
#include "store/db.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::vector<std::string> opened;
    std::function<void(const ItemRow&)> open = [this](const ItemRow& r) { opened.push_back(r.key); };
    Canvas canvas{1236, 1547};
    explicit Rig(int n = 40) { fillFixtureLibrary(lib, n); }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

void tap(LibraryScreen& s, Rect r, Nav& nav) { s.onGesture(tapAt(r.center()), nav); }

// The cell of the jump sheet with that label, on its current page.
std::optional<Rect> cell(const JumpSheet& sheet, const std::string& label) {
    for (const auto& [l, r] : sheet.cells(LibraryScreen::sheetArea()))
        if (l == label) return r;
    return std::nullopt;
}

std::vector<std::string> labels(const std::vector<JumpGroup>& groups) {
    std::vector<std::string> out;
    for (const auto& g : groups)
        for (const auto& e : g.entries) out.push_back(e.label);
    return out;
}

std::vector<ItemRow> everything(Library& lib, ListQuery q) {
    q.offset = 0;
    q.limit = 100000;
    return lib.rows(q);
}

}  // namespace

TEST_CASE("library nav ui: by default the list is the items with a file, All, by Recent") {
    Rig rig;
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    CHECK(screen.view() == LibraryView{});
    CHECK(screen.view().type == TypeFilter::WithFile);
    CHECK(screen.list().pager().total() == 28);  // 14 EPUB + 13 PDF + the standalone PDF, of 41
    CHECK(screen.filterLine() == "28 items");
    CHECK_FALSE(screen.view().filtered());
    for (const auto& r : screen.list().rows()) CHECK_FALSE(r.badge.empty());
}

TEST_CASE("library nav ui: All | Downloaded switches the list") {
    Rig rig;
    rig.lib.setFileState("EP000003", AttachmentRecord::State::Local, "/mnt/us/zotero/files/EP000003/book.epub");
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    rig.nav.damages.clear();
    tap(screen, LibraryScreen::segmentRect(1), rig.nav);
    CHECK(screen.view().downloaded);
    CHECK(screen.list().pager().total() == 1);
    REQUIRE(screen.list().rows().size() == 1);
    CHECK(screen.list().rows()[0].key == "IT000003");
    CHECK(screen.filterLine() == "1 item");
    REQUIRE_FALSE(rig.nav.damages.empty());
    for (const auto& d : rig.nav.damages) CHECK(d.waveform != Waveform::Full);
    tap(screen, LibraryScreen::segmentRect(0), rig.nav);
    CHECK_FALSE(screen.view().downloaded);
    CHECK(screen.list().pager().total() == 28);
}

TEST_CASE("library nav ui: the Filter sheet sets Type, Read and Tag; Done applies, Reset and Clear go back") {
    Rig rig;
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    tap(screen, LibraryScreen::filterButtonRect(), rig.nav);
    REQUIRE(screen.filterSheet());
    const Rect area = LibraryScreen::sheetArea();
    auto type = screen.filterSheet()->typeRects(area);
    REQUIRE(type.size() == 4);  // Any · With a file · PDF · EPUB
    tap(screen, type[2], rig.nav);
    auto read = screen.filterSheet()->readRects(area);
    REQUIRE(read.size() == 3);  // Any · Unread · Read
    tap(screen, read[1], rig.nav);
    CHECK(screen.list().pager().total() == 28);  // nothing applied before Done
    // Tag: a searchable list, most used first; tapping one chooses it.
    auto tag = screen.filterSheet()->tagRects(area);
    REQUIRE(tag.size() == 2);  // None · Choose a tag…
    tap(screen, tag[1], rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* picker = dynamic_cast<TagPickerScreen*>(rig.nav.pushed[0].get());
    REQUIRE(picker);
    picker->onShow(rig.nav);
    REQUIRE(picker->suggestions().size() == 3);
    CHECK(picker->suggestions()[0].first == "economics");
    for (char c : std::string("to")) picker->onKey(KeyEvent{KeyEvent::Code::Text, std::string(1, c)}, rig.nav);
    REQUIRE(picker->suggestions().size() == 1);
    picker->onGesture(tapAt(picker->suggestionRects()[0].center()), rig.nav);
    CHECK(rig.nav.pops == 1);
    REQUIRE(screen.filterSheet());
    CHECK(screen.filterSheet()->draft().tag == "to-read");
    tap(screen, screen.filterSheet()->doneRect(area), rig.nav);
    CHECK_FALSE(screen.filterSheet());
    CHECK(screen.view().type == TypeFilter::Pdf);
    CHECK(screen.view().read == ReadFilter::Unread);
    CHECK(screen.view().tag == "to-read");
    ListQuery q;
    q.type = TypeFilter::Pdf;
    q.read = ReadFilter::Unread;
    q.tag = "to-read";
    const int64_t n = rig.lib.countRows(q);
    CHECK(n > 0);
    CHECK(screen.list().pager().total() == n);
    CHECK(screen.filterLine() == "PDF · Unread · tag: to-read · " + std::to_string(n) + (n == 1 ? " item" : " items"));
    CHECK(screen.view().filtered());

    // Reset in the sheet puts back the defaults (applied with Done).
    tap(screen, LibraryScreen::filterButtonRect(), rig.nav);
    REQUIRE(screen.filterSheet());
    tap(screen, screen.filterSheet()->resetRect(area), rig.nav);
    CHECK(screen.filterSheet()->draft() == LibraryView{});
    tap(screen, {0, LibraryScreen::sheetArea().y + 10, 100, 10}, rig.nav);  // outside: closed, nothing applied
    CHECK_FALSE(screen.filterSheet());
    CHECK(screen.view().tag == "to-read");

    // Clear, on the filter line.
    tap(screen, LibraryScreen::clearRect(), rig.nav);
    CHECK_FALSE(screen.view().filtered());
    CHECK(screen.filterLine() == "28 items");
    CHECK(screen.list().pager().total() == 28);
}

TEST_CASE("library nav ui: the filter line names what differs from the default") {
    LibraryView v;
    CHECK(filterLine(v, 2153) == "2,153 items");
    v.type = TypeFilter::Any;
    CHECK(filterLine(v, 3076) == "All types · 3,076 items");
    v.type = TypeFilter::Epub;
    v.read = ReadFilter::Read;
    CHECK(filterLine(v, 1) == "EPUB · Read · 1 item");
    v.downloaded = true;  // the toggle shows that; Clear leaves it
    v.sort = SortBy::Title;
    CHECK(v.filtered());
    LibraryView cleared = v.cleared();
    CHECK_FALSE(cleared.filtered());
    CHECK(cleared.downloaded);
    CHECK(cleared.sort == SortBy::Title);
}

TEST_CASE("library nav ui: the Sort sheet's five sorts, each with its headings") {
    Rig rig(120);
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    tap(screen, LibraryScreen::sortButtonRect(), rig.nav);
    REQUIRE(screen.sortSheet());
    auto options = screen.sortSheet()->optionRects(LibraryScreen::sheetArea());
    REQUIRE(options.size() == 5);  // Recent · Date added · Title · Author · Year
    CHECK(screen.list().headings(LibraryScreen::listArea()).empty());  // Recent: none

    struct Want {
        int option;
        SortBy sort;
        std::string firstHeading;
    };
    for (const Want& w : {Want{2, SortBy::Title, "A"}, Want{3, SortBy::Author, "A"}, Want{4, SortBy::Year, "2025"},
                          Want{1, SortBy::Added, "May 2000"}}) {
        CAPTURE(w.option);
        if (!screen.sortSheet()) tap(screen, LibraryScreen::sortButtonRect(), rig.nav);
        REQUIRE(screen.sortSheet());
        tap(screen, screen.sortSheet()->optionRects(LibraryScreen::sheetArea())[static_cast<size_t>(w.option)], rig.nav);
        CHECK_FALSE(screen.sortSheet());
        CHECK(screen.view().sort == w.sort);
        CHECK(screen.list().pager().page() == 0);
        auto headings = screen.list().headings(LibraryScreen::listArea());
        REQUIRE_FALSE(headings.empty());
        CHECK(headings[0].first == w.firstHeading);
        // A heading takes half a row, above the first row of its section.
        auto rows = screen.list().rowRects(LibraryScreen::listArea());
        CHECK(headings[0].second.h == kRowH / 2);
        CHECK(rows[0].y == headings[0].second.y + headings[0].second.h);
        // Every row of the list is on exactly one page, in order.
        ListQuery q = screen.query();
        auto all = everything(rig.lib, q);
        std::vector<std::string> paged;
        for (int p = 0; p < screen.list().pager().pages(); ++p) {
            screen.list().goToPage(p, rig.lib);
            for (const auto& r : screen.list().rows()) paged.push_back(r.key);
            CHECK(static_cast<int>(screen.list().rowRects(LibraryScreen::listArea()).size()) <= screen.list().rowsPerPage());
            auto rects = screen.list().rowRects(LibraryScreen::listArea());
            if (!rects.empty())
                CHECK(rects.back().y + rects.back().h <= LibraryScreen::listArea().y + LibraryScreen::listArea().h - kPageIndicatorH);
        }
        std::vector<std::string> keys;
        for (const auto& r : all) keys.push_back(r.key);
        CHECK(paged == keys);
        screen.list().goToPage(0, rig.lib);
    }
    tap(screen, LibraryScreen::sortButtonRect(), rig.nav);
    tap(screen, screen.sortSheet()->optionRects(LibraryScreen::sheetArea())[0], rig.nav);
    CHECK(screen.view().sort == SortBy::Recent);
    CHECK(screen.list().headings(LibraryScreen::listArea()).empty());
}

TEST_CASE("library nav ui: the jump index has only the letters there are, grouped by script, then #") {
    std::vector<ListSection> sections = {{"1A", "A", 3}, {"1D", "D", 2}, {"1Z", "Z", 1}, {"2Б", "Б", 4},
                                         {"2Ф", "Ф", 1}, {"3#", "#", 2}};
    auto groups = jumpGroups(SortBy::Title, sections);
    REQUIRE(groups.size() == 3);
    CHECK(labels({groups[0]}) == std::vector<std::string>{"A", "D", "Z"});
    CHECK(labels({groups[1]}) == std::vector<std::string>{"Б", "Ф"});
    CHECK(labels({groups[2]}) == std::vector<std::string>{"#"});
    CHECK(groups[0].entries[1].section == 1);
    CHECK(groups[2].entries[0].section == 5);
    // Years newest first, then those with none; months in their years.
    auto years = jumpGroups(SortBy::Year, {{"2025", "2025", 1}, {"1999", "1999", 1}, {"", "No year", 3}});
    CHECK(labels(years) == std::vector<std::string>{"2025", "1999", "No year"});
    auto months = jumpGroups(SortBy::Added, {{"2025-03", "March 2025", 1}, {"2025-01", "January 2025", 1}, {"2024-12", "December 2024", 1}});
    REQUIRE(months.size() == 2);
    CHECK(months[0].label == "2025");
    CHECK(labels({months[0]}) == std::vector<std::string>{"Mar", "Jan"});
    CHECK(months[1].label == "2024");
    CHECK(jumpGroups(SortBy::Recent, sections).empty());
}

TEST_CASE("library nav ui: tapping a letter opens the page holding its first item") {
    Rig rig(200);
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    LibraryView v = screen.view();
    v.sort = SortBy::Title;
    screen.setView(v, rig.nav);
    REQUIRE(screen.list().pager().pages() > 3);
    rig.nav.damages.clear();
    tap(screen, LibraryScreen::footerRect(), rig.nav);
    REQUIRE(screen.jumpSheet());
    CHECK(labels(screen.jumpSheet()->groups()) == std::vector<std::string>{"A", "D", "L", "N", "P", "R", "U", "Ф", "#"});
    for (const auto& d : rig.nav.damages) CHECK(d.waveform == Waveform::Ui);
    auto all = everything(rig.lib, screen.query());
    for (const std::string letter : {"Ф", "U", "#", "A"}) {
        CAPTURE(letter);
        if (!screen.jumpSheet()) tap(screen, LibraryScreen::footerRect(), rig.nav);
        auto r = cell(*screen.jumpSheet(), letter);
        REQUIRE(r);
        rig.nav.damages.clear();
        tap(screen, *r, rig.nav);
        CHECK_FALSE(screen.jumpSheet());
        size_t first = 0;
        while (first < all.size() && listSortKey(all[first].title).substr(1, std::string(letter).size()) != letter) ++first;
        REQUIRE(first < all.size());
        const int64_t offset = screen.list().pager().offset();
        CHECK(offset <= static_cast<int64_t>(first));
        CHECK(static_cast<int64_t>(first) < offset + static_cast<int64_t>(screen.list().rows().size()));
        auto headings = screen.list().headings(LibraryScreen::listArea());
        bool shown = false;
        for (const auto& h : headings) shown |= h.first == letter;
        CHECK(shown);
        for (const auto& d : rig.nav.damages) CHECK(d.waveform != Waveform::Full);
    }
}

TEST_CASE("library nav ui: by Recent the jump sheet has First page and Go to page") {
    Rig rig(120);
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    const int pages = screen.list().pager().pages();
    REQUIRE(pages > 4);
    screen.onGesture(swipe(Direction::Up), rig.nav);
    screen.onGesture(swipe(Direction::Up), rig.nav);
    tap(screen, LibraryScreen::footerRect(), rig.nav);
    REQUIRE(screen.jumpSheet());
    CHECK(screen.jumpSheet()->groups().empty());
    const Rect area = LibraryScreen::sheetArea();
    tap(screen, screen.jumpSheet()->firstPageRect(area), rig.nav);
    CHECK(screen.list().pager().page() == 0);
    CHECK_FALSE(screen.jumpSheet());
    tap(screen, LibraryScreen::footerRect(), rig.nav);
    tap(screen, screen.jumpSheet()->pageFieldRect(area), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* pad = dynamic_cast<GoToPageScreen*>(rig.nav.pushed[0].get());
    REQUIRE(pad);
    CHECK_FALSE(pad->isReading());  // the Library stays upright
    for (const char* k : {"4", "Go"}) pad->onGesture(tapAt(GoToPageScreen::keyRect(k).center()), rig.nav);
    CHECK(rig.nav.pops == 1);
    CHECK(screen.list().pager().page() == 3);
    CHECK(screen.list().pager().offset() == 3 * screen.list().rowsPerPage());
    CHECK_FALSE(screen.jumpSheet());
}

TEST_CASE("library nav ui: the view is remembered on this device") {
    Rig rig;
    {
        LibraryScreen screen(rig.open);
        screen.onShow(rig.nav);
        LibraryView v;
        v.downloaded = true;
        v.type = TypeFilter::Epub;
        v.read = ReadFilter::Unread;
        v.tag = "pinn";
        v.sort = SortBy::Author;
        screen.setView(v, rig.nav);
    }
    LibraryScreen again(rig.open);
    again.onShow(rig.nav);
    CHECK(again.view().downloaded);
    CHECK(again.view().type == TypeFilter::Epub);
    CHECK(again.view().read == ReadFilter::Unread);
    CHECK(again.view().tag == "pinn");
    CHECK(again.view().sort == SortBy::Author);
    rig.lib.setMetaValue("libraryView", "not json");
    LibraryScreen broken(rig.open);
    broken.onShow(rig.nav);
    CHECK(broken.view() == LibraryView{});
}

TEST_CASE("library nav ui: the header, sheets and jump letters match the reviewed goldens") {
    if (!fonts()) return;
    Rig rig(200);
    rig.lib.markRead("IT000004", true);
    LibraryScreen screen(rig.open);
    screen.onShow(rig.nav);
    LibraryView v;
    v.sort = SortBy::Title;
    v.read = ReadFilter::Unread;
    screen.setView(v, rig.nav);
    screen.draw(rig.canvas);
    checkGolden(rig.canvas, "tests/golden/library-sections.png");
    tap(screen, LibraryScreen::filterButtonRect(), rig.nav);
    screen.draw(rig.canvas);
    checkGolden(rig.canvas, "tests/golden/library-filter-sheet.png");
    tap(screen, {0, LibraryScreen::sheetArea().y + 10, 100, 10}, rig.nav);
    tap(screen, LibraryScreen::sortButtonRect(), rig.nav);
    screen.draw(rig.canvas);
    checkGolden(rig.canvas, "tests/golden/library-sort-sheet.png");
    tap(screen, {0, LibraryScreen::sheetArea().y + 10, 100, 10}, rig.nav);
    tap(screen, LibraryScreen::footerRect(), rig.nav);
    REQUIRE(screen.jumpSheet());
    screen.draw(rig.canvas);
    checkGolden(rig.canvas, "tests/golden/library-jump-sheet.png");
}

// The real library, when a copy is given (ZK_REAL_LIBRARY=/tmp/zk.sqlite: a copy, as it is migrated).
TEST_CASE("library nav ui: the real library's jump letters, and a swipe and the jump sheet stay fast") {
    const char* path = std::getenv("ZK_REAL_LIBRARY");
    if (!path) return;
    Db db = Db::open(path);
    Library lib(db);
    FakeNav nav(lib);
    LibraryScreen screen(nullptr);
    auto ms = [](auto&& f) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    MESSAGE("first show " << ms([&] { screen.onShow(nav); }) << " ms, " << screen.filterLine() << ", "
                          << screen.list().pager().pages() << " pages");
    for (SortBy sort : {SortBy::Recent, SortBy::Title, SortBy::Author, SortBy::Year, SortBy::Added}) {
        LibraryView v = screen.view();
        v.sort = sort;
        const double apply = ms([&] { screen.setView(v, nav); });
        const double swipe = ms([&] { screen.onGesture(zk::test::swipe(Direction::Up), nav); });
        const double open = ms([&] { screen.onGesture(tapAt(LibraryScreen::footerRect().center()), nav); });
        REQUIRE(screen.jumpSheet());
        std::string cells;
        for (const auto& [l, r] : screen.jumpSheet()->cells(LibraryScreen::sheetArea())) cells += l + " ";
        MESSAGE(std::string(sortLabel(sort)) << ": apply " << apply << " ms, swipe " << swipe << " ms, jump sheet " << open << " ms, "
                                << screen.list().pager().pages() << " pages, sheet pages "
                                << screen.jumpSheet()->sheetPages(LibraryScreen::sheetArea()) << ": " << cells);
        CHECK(swipe < 50);
        CHECK(open < 50);
        if (sort == SortBy::Title && fonts()) {
            Canvas c(1236, 1547);
            screen.draw(c);
            cairo_surface_write_to_png(c.surface(), "/tmp/zklib/real-jump-title.png");
        }
        if (sort == SortBy::Title) {  // a Cyrillic letter lands on its first item
            auto r = cell(*screen.jumpSheet(), "Ж");
            REQUIRE(r);
            screen.onGesture(tapAt(r->center()), nav);
            bool shown = false;
            for (const auto& h : screen.list().headings(LibraryScreen::listArea())) shown |= h.first == "Ж";
            CHECK(shown);
            if (fonts()) {
                Canvas c(1236, 1547);
                screen.draw(c);
                cairo_surface_write_to_png(c.surface(), "/tmp/zklib/real-title-zh.png");
            }
        } else {
            screen.onGesture(tapAt({5, 5}), nav);  // on the title: closes even a sheet filling the area
            CHECK_FALSE(screen.jumpSheet());
        }
    }
}
