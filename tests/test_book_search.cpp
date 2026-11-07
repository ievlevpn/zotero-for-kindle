#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/book_search.h"
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
    std::unique_ptr<ReaderScreen> reader;
    Rig() {
        fillFixtureLibrary(lib, 10);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book", DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(new FakeEngine()); });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        tick([&] { return reader->ready(); });
    }
    bool tick(std::function<bool()> done, Screen* s = nullptr) {
        for (int i = 0; i < 400; ++i) {
            (s ? s : reader.get())->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    BookSearchScreen* openSearch() {
        reader->onGesture(tapAt({618, 700}), nav);
        reader->onGesture(tapAt(ReaderMenus::searchRect().center()), nav);
        REQUIRE(nav.pushed.size() == 1);
        auto* s = dynamic_cast<BookSearchScreen*>(nav.pushed[0].get());
        REQUIRE(s);
        nav.visible = s;
        s->onShow(nav);
        return s;
    }
    void type(Screen* s, const std::string& text) {
        for (char c : text) s->onKey({KeyEvent::Code::Text, std::string(1, c)}, nav);
        s->onKey({KeyEvent::Code::Return, ""}, nav);
    }
};

}  // namespace

TEST_CASE("book search: Return searches on the worker; tapping a hit goes there") {
    Rig rig;
    auto* s = rig.openSearch();
    CHECK(rig.nav.keyboardFake.shown);
    rig.type(s, "Bingley");
    CHECK(s->searching());
    REQUIRE(rig.tick([&] { return !s->searching(); }, s));
    REQUIRE(s->hits().size() >= 2);
    const int target = s->hits()[1].page;
    s->onGesture(tapAt(s->hitRects()[1].center()), rig.nav);
    CHECK(rig.nav.pops == 1);
    CHECK_FALSE(rig.nav.keyboardFake.shown);
    rig.nav.visible = rig.reader.get();
    CHECK(rig.tick([&] { return rig.reader->page() == target; }));
}

TEST_CASE("book search: no hits says so") {
    Rig rig;
    auto* s = rig.openSearch();
    rig.type(s, "zzz");
    REQUIRE(rig.tick([&] { return !s->searching(); }, s));
    CHECK(s->hits().empty());
    CHECK(s->emptyMessage() == "Not found in this book.");
}

TEST_CASE("book search matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    auto* s = rig.openSearch();
    rig.type(s, "Bingley");
    REQUIRE(rig.tick([&] { return !s->searching(); }, s));
    Canvas c(1236, 1547);
    s->draw(c);
    test::checkGolden(c, "tests/golden/book-search.png");
}
