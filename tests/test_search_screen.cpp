#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/search.h"
#include "store/db.h"
#include "ui/text.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::vector<std::string> opened;
    SearchScreen screen{[this](const ItemRow& r) { opened.push_back(r.key); }};
    Rig() {
        fillFixtureLibrary(lib, 40);
        screen.onShow(nav);
        nav.damages.clear();
    }
    void type(const std::string& s) {
        // One key per UTF-8 code point, as the keyboard sends them.
        for (size_t i = 0; i < s.size();) {
            size_t n = 1;
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c >= 0xF0) n = 4;
            else if (c >= 0xE0) n = 3;
            else if (c >= 0xC0) n = 2;
            screen.onKey({KeyEvent::Code::Text, s.substr(i, n)}, nav);
            i += n;
        }
    }
    int count(Waveform w) {
        int n = 0;
        for (const auto& d : nav.damages) n += d.waveform == w;
        return n;
    }
};

}  // namespace

TEST_CASE("search: keys echo fast in the field; results wait for a pause") {
    Rig rig;
    rig.type("neur");
    CHECK(rig.count(Waveform::Fast) == 4);
    CHECK(rig.count(Waveform::Quality) == 0);
    for (const auto& d : rig.nav.damages) CHECK(d.rect == SearchScreen::fieldRect());
    rig.nav.now += 300;
    rig.screen.onTick(rig.nav);
    CHECK(rig.count(Waveform::Quality) == 0);
    rig.nav.now += 400;
    rig.screen.onTick(rig.nav);
    CHECK(rig.count(Waveform::Quality) == 1);
    REQUIRE_FALSE(rig.screen.results().rows().empty());
    CHECK(rig.screen.results().rows()[0].title.find("neural") != std::string::npos);
    rig.nav.now += 5000;
    rig.screen.onTick(rig.nav);
    CHECK(rig.count(Waveform::Quality) == 1);  // nothing new typed: no extra refresh
}

TEST_CASE("search: Return searches at once; Cyrillic works; no results says so") {
    Rig rig;
    rig.type("Филос");
    rig.screen.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    REQUIRE_FALSE(rig.screen.results().rows().empty());
    CHECK(rig.screen.results().rows()[0].title.find("Философия") == 0);
    for (int i = 0; i < 5; ++i) rig.screen.onKey({KeyEvent::Code::Backspace, ""}, rig.nav);
    rig.type("zzzzqq");
    rig.screen.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    CHECK(rig.screen.results().rows().empty());
    CHECK(rig.screen.emptyMessage() == "Nothing found for “zzzzqq”");
}

TEST_CASE("search: the keyboard opens with the screen and closes on Escape or a result") {
    Rig rig;
    CHECK(rig.nav.keyboardFake.shown);
    rig.type("neur");
    rig.screen.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    auto rects = rig.screen.results().rowRects(rig.screen.resultsArea(rig.nav));
    REQUIRE_FALSE(rects.empty());
    rig.screen.onGesture(tapAt(rects[0].center()), rig.nav);
    CHECK_FALSE(rig.nav.keyboardFake.shown);
    REQUIRE(rig.opened.size() == 1);
    rig.screen.onGesture(tapAt(SearchScreen::fieldRect().center()), rig.nav);
    CHECK(rig.nav.keyboardFake.shown);
    rig.screen.onKey({KeyEvent::Code::Escape, ""}, rig.nav);
    CHECK_FALSE(rig.nav.keyboardFake.shown);
    CHECK(rig.nav.pops == 1);
}

TEST_CASE("search: results stay above the keyboard; a note says highlights are on demand") {
    Rig rig;
    CHECK(rig.screen.resultsArea(rig.nav).y + rig.screen.resultsArea(rig.nav).h <= 980);
    CHECK_FALSE(rig.screen.note(rig.nav).empty());
    rig.lib.setAllAnnotationsOffline(true);
    CHECK(rig.screen.note(rig.nav).empty());
}

TEST_CASE("search screen matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    rig.type("economics");
    rig.screen.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    Canvas c(1236, 1547);
    rig.screen.draw(c);
    test::checkGolden(c, "tests/golden/search.png");
}
