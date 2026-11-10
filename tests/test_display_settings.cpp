#include <doctest/doctest.h>

#include "docs/fonts.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/text.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    Rig() { fillFixtureLibrary(lib, 10); }
    std::unique_ptr<ReaderScreen> open(DocType type = DocType::Epub) {
        auto r = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = r.get();
        r->onShow(nav);
        return r;
    }
    bool waitPage(ReaderScreen& r, int page) {
        for (int i = 0; i < 400; ++i) {
            r.onTick(nav);
            if (r.page() == page && r.ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void openDisplay(ReaderScreen& r) {
        r.onGesture(tapAt({metrics().width / 2, metrics().height / 2}), nav);  // the menus
        REQUIRE(r.menusOpen());
        r.onGesture(tapAt(ReaderMenus::buttonRect(2, r.menus()->buttonCount()).center()), nav);
    }
};

}  // namespace

TEST_CASE("fonts: family names from the font files, Bookerly first") {
    auto fonts = readingFonts({"assets/fonts-device"});
    REQUIRE_FALSE(fonts.empty());
    CHECK(fonts[0] == "Bookerly");
    CHECK(std::find(fonts.begin(), fonts.end(), "Amazon Ember") != fonts.end());
    CHECK(std::adjacent_find(fonts.begin(), fonts.end()) == fonts.end());  // no duplicates
    CHECK(readingFonts({"/nonexistent"}).empty());
}

TEST_CASE("display: a bigger size keeps the passage and is remembered for the book") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    r->turnTo(40);
    REQUIRE(rig.waitPage(*r, 40));
    rig.openDisplay(*r);
    REQUIRE(r->menus()->displayOpen());
    r->onGesture(tapAt(ReaderMenus::sizePlusRect().center()), rig.nav);
    CHECK(r->layout().sizePt == doctest::Approx(9.5));
    CHECK(rig.waitPage(*r, 38));  // the same position, re-paginated (FakeEngine: 40 × 9 / 9.5)
    auto again = rig.open();
    CHECK(again->layout().sizePt == doctest::Approx(9.5));
}

TEST_CASE("display: margins, spacing and font choices apply; PDFs only get margins") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::marginRect(2).center()), rig.nav);
    CHECK(r->layout().marginPx == 100);
    r->onGesture(tapAt(ReaderMenus::spacingRect(0).center()), rig.nav);
    CHECK(r->layout().lineSpacingPct == 100);
    auto pdf = rig.open(DocType::Pdf);
    REQUIRE(rig.waitPage(*pdf, 0));
    rig.openDisplay(*pdf);
    CHECK_FALSE(pdf->menus()->showsTextOptions());
}

TEST_CASE("display panel matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    Canvas c(1236, 1547);
    r->draw(c);
    test::checkGolden(c, "tests/golden/display-panel.png");
}

TEST_CASE("fonts: from the Kindle's system fonts only its reading fonts are offered; added fonts all are") {
    for (const char* f : {"Bookerly", "Amazon Ember", "Baskerville", "Caecilia Regular", "Futura", "Helvetica Neue LT",
                          "OpenDyslexic", "Palatino"})
        CHECK_MESSAGE(offeredFamily(f, true), f);
    for (const char* f : {"Kindle Symbol", "STSong", "Noto Sans Tamil", "condensed", "Sunroom Mono", "Cadia Single",
                          "Amazon Ember Bold", "Bookerly Display", "MT Chinese Surrogates"})
        CHECK_FALSE_MESSAGE(offeredFamily(f, true), f);
    CHECK(offeredFamily("Charter", false));  // dropped into /mnt/us/zotero/fonts
}

// ---- The font pop-up ----

namespace {

const Rect* rowFor(const std::vector<std::pair<std::string, Rect>>& rows, const std::string& font) {
    for (const auto& [f, r] : rows)
        if (f == font) return &r;
    return nullptr;
}

}  // namespace

TEST_CASE("font pop-up: the Font row opens the list; a tap picks a font and closes it") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    ReaderMenus& m = *r->menus();
    CHECK_FALSE(m.fontPickerOpen());
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    REQUIRE(m.fontPickerOpen());
    const auto rows = m.fontPickerRows();
    REQUIRE(rowFor(rows, "Bookerly"));
    REQUIRE(rowFor(rows, "Fast Serif"));
    for (const auto& [f, rect] : rows) CHECK(m.fontPickerRect().contains(rect.center()));
    r->onGesture(tapAt(rowFor(rows, "Fast Serif")->center()), rig.nav);
    CHECK(r->layout().font == "Fast Serif");
    CHECK_FALSE(m.fontPickerOpen());
    CHECK(m.displayOpen());  // back to the Display panel
}

TEST_CASE("font pop-up: the Fast fonts come last") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    const auto& all = r->menus()->fonts();
    const auto firstFast = std::find_if(all.begin(), all.end(), [](const std::string& f) { return f.rfind("Fast ", 0) == 0; });
    REQUIRE(firstFast != all.end());
    CHECK(std::all_of(firstFast, all.end(), [](const std::string& f) { return f.rfind("Fast ", 0) == 0; }));
}

TEST_CASE("font pop-up: a tap outside the list closes only the list") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    const std::string before = r->layout().font;
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    REQUIRE(r->menus()->fontPickerOpen());
    r->onGesture(tapAt({5, 5}), rig.nav);
    CHECK_FALSE(r->menus()->fontPickerOpen());
    CHECK(r->menusOpen());
    CHECK(r->menus()->displayOpen());
    CHECK(r->layout().font == before);
}

namespace {

// What a Kindle offers: its reading fonts, then Fast Font.
struct KindleFonts {
    KindleFonts() {
        ReaderMenus::setFontsForTests({"Bookerly", "Amazon Ember", "Baskerville", "Caecilia", "Futura", "Helvetica Neue LT",
                                       "OpenDyslexic", "Palatino", "Fast Serif", "Fast Sans", "Fast Mono"});
    }
    ~KindleFonts() { ReaderMenus::setFontsForTests({}); }
};

}  // namespace

TEST_CASE("font pop-up: a Kindle's eleven fonts page, on the PW5 and the KT4") {
    for (const Metrics& screen : {kPw5, kKt4}) {
    INFO(screen.device);
    ScopedMetrics s(screen);
    KindleFonts kindle;
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    ReaderMenus& m = *r->menus();
    REQUIRE(m.fontPickerOpen());
    CHECK(m.fontPickerRect().y >= 0);
    CHECK(m.fontPickerRect().y + m.fontPickerRect().h <= ReaderMenus::panelRect().y);
    const auto first = m.fontPickerRows();
    REQUIRE(first.size() < m.fonts().size());
    for (const auto& [f, rect] : first) CHECK(rect.y + rect.h <= m.fontPickerRect().y + m.fontPickerRect().h - 100);
    CHECK(m.fontPickerPrevRect().empty());
    REQUIRE_FALSE(m.fontPickerNextRect().empty());
    r->onGesture(tapAt(m.fontPickerNextRect().center()), rig.nav);
    REQUIRE(m.fontPickerOpen());
    const auto second = m.fontPickerRows();
    REQUIRE_FALSE(second.empty());
    CHECK(second.front().first != first.front().first);
    r->onGesture(tapAt(m.fontPickerPrevRect().center()), rig.nav);
    CHECK(m.fontPickerRows().front().first == first.front().first);
    }
}

TEST_CASE("font pop-up matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    addFontDir("assets/fonts-fast");
    KindleFonts kindle;
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    Canvas c(1236, 1547);
    r->draw(c);
    test::checkGolden(c, "tests/golden/font-picker.png");
    r->onGesture(tapAt(r->menus()->fontPickerNextRect().center()), rig.nav);
    r->draw(c);
    test::checkGolden(c, "tests/golden/font-picker-fast.png");
}

TEST_CASE("font pop-up: the Fast fonts as found are in the order Serif, Sans, Mono") {
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    const auto& all = r->menus()->fonts();
    const auto serif = std::find(all.begin(), all.end(), "Fast Serif");
    REQUIRE(serif != all.end());
    REQUIRE(serif + 2 < all.end());
    CHECK(*(serif + 1) == "Fast Sans");
    CHECK(*(serif + 2) == "Fast Mono");
}

TEST_CASE("fonts: only the bundled Fast fonts count as Fast") {
    CHECK(isFastFont("Fast Serif"));
    CHECK(isFastFont("Fast Mono"));
    CHECK_FALSE(isFastFont("Fast Hand"));  // a user's own font with a similar name
    CHECK_FALSE(isFastFont("Bookerly"));
}

TEST_CASE("font pop-up: the Fast group isn't split across pages where a page can hold it (Scribe landscape)") {
    Metrics landscape = kScribe;
    landscape.width = 2480;
    landscape.height = 1759;
    ScopedMetrics s(landscape);
    KindleFonts kindle;
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    ReaderMenus& m = *r->menus();
    REQUIRE(m.fontPickerOpen());
    for (int page = 0; page < 6; ++page) {
        const auto rows = m.fontPickerRows();
        if (rowFor(rows, "Fast Serif")) {
            CHECK(rowFor(rows, "Fast Sans"));
            CHECK(rowFor(rows, "Fast Mono"));
            return;
        }
        REQUIRE_FALSE(m.fontPickerNextRect().empty());
        r->onGesture(tapAt(m.fontPickerNextRect().center()), rig.nav);
    }
    FAIL("no page with Fast Serif");
}

TEST_CASE("font pop-up: a tap inside it that misses a row or an arrow keeps it open") {
    KindleFonts kindle;
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    ReaderMenus& m = *r->menus();
    const Rect box = m.fontPickerRect();
    r->onGesture(tapAt({box.x + box.w / 2, box.y + 40}), rig.nav);  // the title
    CHECK(m.fontPickerOpen());
    r->onGesture(tapAt({box.x + box.w / 2, box.y + box.h - 50}), rig.nav);  // "1 / 2"
    REQUIRE(m.fontPickerOpen());
    REQUIRE_FALSE(m.fontPickerRows().empty());
    CHECK(m.fontPickerRows().front().first == "Bookerly");  // still the first page
}

TEST_CASE("font pop-up: a screen that grows while it is open shows a page that exists") {
    KindleFonts kindle;
    std::optional<ScopedMetrics> small(std::in_place, kKt4);
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    ReaderMenus& m = *r->menus();
    while (!m.fontPickerNextRect().empty()) r->onGesture(tapAt(m.fontPickerNextRect().center()), rig.nav);
    small.reset();
    ScopedMetrics big(kScribe);  // everything fits on one page now
    CHECK(m.fontPickerPrevRect().empty());
    CHECK(m.fontPickerNextRect().empty());
    CHECK(m.fontPickerRows().size() == m.fonts().size());
}

TEST_CASE("font pop-up: where the Fast group can't fit on a page, its heading comes again (KT4 landscape)") {
    Metrics landscape = kKt4;
    std::swap(landscape.width, landscape.height);
    ScopedMetrics s(landscape);
    KindleFonts kindle;
    Rig rig;
    auto r = rig.open();
    REQUIRE(rig.waitPage(*r, 0));
    rig.openDisplay(*r);
    r->onGesture(tapAt(ReaderMenus::fontRowRect().center()), rig.nav);
    ReaderMenus& m = *r->menus();
    int fastPages = 0;
    for (int page = 0; page < 8; ++page) {
        const auto rows = m.fontPickerRows();
        const bool fast = std::any_of(rows.begin(), rows.end(), [](const auto& row) { return isFastFont(row.first); });
        if (fast) {
            ++fastPages;
            CHECK(m.fontPickerHeadingShown());
        }
        for (const auto& [f, rect] : rows) CHECK(rect.y + rect.h <= m.fontPickerRect().y + m.fontPickerRect().h - 100);
        if (m.fontPickerNextRect().empty()) break;
        r->onGesture(tapAt(m.fontPickerNextRect().center()), rig.nav);
    }
    CHECK(fastPages >= 2);
}
