// The Notes tab's search: highlights and notes whose text, comment or tags match.
#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/notes.h"
#include "screens/search.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::vector<std::string> opened;  // "<item>/<annotation>"
    Rig() { fillFixtureLibrary(lib, 40); }
    NotesScreen::OpenBook open() {
        return [this](const ItemRow& r, std::optional<AnnotationRecord> at) {
            opened.push_back(r.key + "/" + (at ? at->key : ""));
        };
    }
    void type(NotesScreen& s, const std::string& text) {
        for (char c : text) s.onKey({KeyEvent::Code::Text, std::string(1, c)}, nav);
    }
    int notes(const NotesScreen& s) {
        int n = 0;
        for (const auto& l : s.page()) n += !l.header;
        return n;
    }
};

}  // namespace

TEST_CASE("notes search: the tab's icon opens it; typing finds the matching highlights, by book") {
    Rig rig;
    NotesScreen tab(rig.open());
    tab.onShow(rig.nav);
    CHECK(tab.showsTabBar());
    tab.onGesture(tapAt(NotesScreen::searchRect().center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* s = dynamic_cast<NotesScreen*>(rig.nav.pushed[0].get());
    REQUIRE(s);
    CHECK(s->searching());
    CHECK_FALSE(s->showsTabBar());
    s->onShow(rig.nav);
    CHECK(rig.nav.keyboardFake.shown);
    CHECK(s->page().empty());  // nothing typed: nothing listed
    rig.type(*s, "highlight 2");
    s->onTick(rig.nav);
    CHECK(s->page().empty());  // not before the pause
    rig.nav.now += NotesScreen::kPauseMs;
    s->onTick(rig.nav);
    CHECK(s->query() == "highlight 2");
    REQUIRE(rig.notes(*s) > 0);
    for (const auto& l : s->page())
        if (!l.header) CHECK(l.note.annotation.text.rfind("highlight 2 ", 0) == 0);
    // Every match is one note of each of the 5 annotated books.
    CHECK(rig.lib.countAnnotations("highlight 2") == 5);
    // A tap opens the book at that highlight.
    const auto rects = s->lineRects();
    size_t i = 0;
    while (s->page()[i].header) ++i;
    s->onGesture(tapAt(rects[i].center()), rig.nav);
    REQUIRE(rig.opened.size() == 1);
    CHECK(rig.opened[0] == s->page()[i].itemKey + "/" + s->page()[i].note.annotation.key);
    CHECK_FALSE(rig.nav.keyboardFake.shown);
}

TEST_CASE("notes search: nothing found says so; Back and Escape leave") {
    Rig rig;
    NotesScreen s(rig.open(), true);
    s.onShow(rig.nav);
    rig.type(s, "zebra");
    s.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    CHECK(s.page().empty());
    s.onKey({KeyEvent::Code::Escape, ""}, rig.nav);
    CHECK(rig.nav.pops == 1);
    NotesScreen t(rig.open(), true);
    t.onShow(rig.nav);
    t.onGesture(tapAt(headerBackRect().center()), rig.nav);  // the header's ‹
    CHECK(rig.nav.pops == 2);
}

TEST_CASE("notes search: the tab and a search match the reviewed goldens") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    Canvas c(1236, 1547);
    NotesScreen tab(rig.open());
    tab.onShow(rig.nav);
    tab.draw(c);
    test::checkGolden(c, "tests/golden/notes-tab.png");
    NotesScreen s(rig.open(), true);
    s.onShow(rig.nav);
    rig.type(s, "highlight 2");
    s.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    s.draw(c);
    test::checkGolden(c, "tests/golden/notes-search.png");
    NotesScreen none(rig.open(), true);
    none.onShow(rig.nav);
    rig.type(none, "zebra");
    none.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    none.draw(c);
    test::checkGolden(c, "tests/golden/notes-search-none.png");
}
