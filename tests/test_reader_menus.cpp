#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_lipc.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/contents.h"
#include "screens/book_notes.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "ui/text.h"

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
    FakeLipc lipc;
    std::unique_ptr<ReaderScreen> reader;
    Rig() {
        fillFixtureLibrary(lib, 10);
        lipc.props["com.lab126.powerd flIntensity"] = "10";
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6, &lipc);
        nav.visible = reader.get();
        reader->onShow(nav);
        waitPage(0);
    }
    bool waitPage(int page) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (reader->page() == page && reader->ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void openMenus() {
        nav.damages.clear();
        reader->onGesture(tapAt({618, 700}), nav);
    }
    Gesture drag(GestureType t, Point p) { return Gesture{t, p, {ReaderMenus::sliderRect().x, p.y}}; }
};

}  // namespace

TEST_CASE("reader menus: a middle tap opens them fast; a tap outside closes them") {
    Rig rig;
    rig.openMenus();
    REQUIRE(rig.reader->menusOpen());
    REQUIRE_FALSE(rig.nav.damages.empty());
    for (const auto& d : rig.nav.damages) {
        CHECK(d.waveform == Waveform::Ui);
        CHECK((d.rect == ReaderMenus::topBarRect() || d.rect == ReaderMenus::panelRect()));
    }
    rig.nav.damages.clear();
    rig.reader->onGesture(tapAt({618, 700}), rig.nav);  // the page, between the bars
    CHECK_FALSE(rig.reader->menusOpen());
    for (const auto& d : rig.nav.damages) CHECK(d.waveform == Waveform::Quality);
}

TEST_CASE("reader menus: dragging the slider to the middle shows that page on release") {
    Rig rig;
    rig.openMenus();
    const Rect s = ReaderMenus::sliderRect();
    rig.reader->onGesture(rig.drag(GestureType::DragStart, {s.x + 10, s.y + s.h / 2}), rig.nav);
    rig.reader->onGesture(rig.drag(GestureType::DragMove, {s.x + s.w / 2, s.y + s.h / 2}), rig.nav);
    CHECK(rig.reader->page() == 0);  // only the label moves while dragging
    rig.reader->onGesture(rig.drag(GestureType::DragEnd, {s.x + s.w / 2, s.y + s.h / 2}), rig.nav);
    CHECK(rig.waitPage(50));
}

TEST_CASE("reader menus: Contents jumps to a chapter; Notes shows the book's count") {
    Rig rig;
    rig.openMenus();
    auto* menus = rig.reader->menus();
    REQUIRE(menus);
    CHECK(menus->notesLabel() == "Notes (3)");
    rig.reader->onGesture(tapAt(ReaderMenus::buttonRect(0).center()), rig.nav);  // Contents
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* contents = dynamic_cast<ContentsScreen*>(rig.nav.pushed[0].get());
    REQUIRE(contents);
    contents->onShow(rig.nav);
    contents->onGesture(tapAt(contents->entryRects()[3].center()), rig.nav);
    CHECK(rig.nav.pops == 1);
    CHECK(rig.waitPage(30));
    rig.openMenus();
    rig.reader->onGesture(tapAt(ReaderMenus::buttonRect(1).center()), rig.nav);  // Notes
    REQUIRE(rig.nav.pushed.size() == 2);
    CHECK(dynamic_cast<BookNotesScreen*>(rig.nav.pushed[1].get()));
}

TEST_CASE("reader menus: Light sets the frontlight; Sync refreshes the book's annotations; back leaves") {
    Rig rig;
    rig.openMenus();
    rig.reader->onGesture(tapAt(ReaderMenus::buttonRect(3).center()), rig.nav);  // Light
    REQUIRE(rig.reader->menus()->lightOpen());
    const Rect l = ReaderMenus::lightSliderRect();
    rig.reader->onGesture(tapAt({l.x + l.w, l.y + l.h / 2}), rig.nav);
    CHECK(rig.lipc.sets.back() == "com.lab126.powerd flIntensity 24");
    rig.reader->onGesture(tapAt(ReaderMenus::buttonRect(4).center()), rig.nav);  // Sync
    REQUIRE(rig.nav.syncFake.opened.size() == 1);
    CHECK(rig.nav.syncFake.opened[0] == "IT000003/EP000003");
    rig.reader->onGesture(tapAt(ReaderMenus::backRect().center()), rig.nav);
    CHECK(rig.nav.pops == 1);
}

TEST_CASE("reader menus: opening and closing Light or Display redraws both cells, so the frame moves at once") {
    Rig rig;
    rig.openMenus();
    const Rect light = ReaderMenus::buttonRect(3), display = ReaderMenus::buttonRect(2);
    auto redrawn = [&](Rect cell) {
        for (const auto& d : rig.nav.damages)
            if (d.rect.contains(cell.center())) return true;
        return false;
    };
    for (Rect tap : {light, light, display, light, display, display}) {  // open, close, switch both ways
        rig.nav.damages.clear();
        rig.reader->onGesture(tapAt(tap.center()), rig.nav);
        CHECK(redrawn(light));
        CHECK(redrawn(display));
    }
}

TEST_CASE("reader menus match the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    for (int p = 1; p <= 12; ++p) {
        rig.reader->turnTo(p);
        REQUIRE(rig.waitPage(p));
    }
    rig.openMenus();
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    test::checkGolden(c, "tests/golden/reader-menus.png");
}
