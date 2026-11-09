// Search in a collection (and its subcollections), and recent searches.
#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/collections.h"
#include "screens/search.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/text.h"

#include <algorithm>
#include <filesystem>
#include <set>

using namespace zk;
using namespace zk::test;

namespace {

std::set<std::string> keys(const std::vector<ItemRow>& rows) {
    std::set<std::string> out;
    for (const auto& r : rows) out.insert(r.key);
    return out;
}

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::vector<std::string> opened;
    Rig() { fillFixtureLibrary(lib, 40); }
    void type(SearchScreen& s, const std::string& text) {
        for (char c : text) s.onKey({KeyEvent::Code::Text, std::string(1, c)}, nav);
    }
};

}  // namespace

TEST_CASE("search in a collection: its items and its subcollections' items, nothing else") {
    Rig rig;
    const auto all = rig.lib.searchRows("sargent", 0, 500);
    std::set<std::string> expected;  // Economics, or Growth inside it
    for (const auto& r : all) {
        const auto item = rig.lib.item(r.key);
        for (const auto& c : item->collections)
            if (c == "COLLAAAA" || c == "COLLBBBB") expected.insert(r.key);
    }
    REQUIRE(expected.size() > 2);
    REQUIRE(expected.size() < all.size());
    CHECK(keys(rig.lib.searchRows("sargent", 0, 500, "COLLAAAA")) == expected);
    // Growth alone: only its own.
    for (const auto& r : rig.lib.searchRows("sargent", 0, 500, "COLLBBBB")) {
        const auto c = rig.lib.item(r.key)->collections;
        CHECK(std::find(c.begin(), c.end(), "COLLBBBB") != c.end());
    }
    CHECK(rig.lib.searchRows("sargent", 0, 500, "NOSUCH00").empty());
}

TEST_CASE("recent searches: newest first, no repeats, eight at most, blank ones ignored") {
    Rig rig;
    CHECK(rig.lib.recentSearches().empty());
    for (int i = 0; i < 10; ++i) rig.lib.rememberSearch("q" + std::to_string(i));
    rig.lib.rememberSearch("q5");
    rig.lib.rememberSearch("   ");
    const auto r = rig.lib.recentSearches();
    REQUIRE(r.size() == Library::kRecentSearches);
    CHECK(r[0] == "q5");
    CHECK(r[1] == "q9");
    CHECK(std::count(r.begin(), r.end(), "q5") == 1);
    rig.lib.clearRecentSearches();
    CHECK(rig.lib.recentSearches().empty());
}

TEST_CASE("search screen: scoped to a collection until its chip's ✕; Return remembers the search") {
    Rig rig;
    SearchScreen s([&](const ItemRow& r) { rig.opened.push_back(r.key); }, {}, "COLLAAAA", "Economics");
    s.onShow(rig.nav);
    CHECK_FALSE(s.scopeRect().empty());
    rig.type(s, "sargent");
    s.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    const auto scoped = rig.lib.searchRows("sargent", 0, 500, "COLLAAAA");
    CHECK(s.results().pager().total() == static_cast<int64_t>(scoped.size()));
    CHECK(rig.lib.recentSearches() == std::vector<std::string>{"sargent"});
    s.onGesture(tapAt(s.scopeRect().center()), rig.nav);
    CHECK(s.scopeKey().empty());
    CHECK(s.scopeRect().empty());
    CHECK(s.results().pager().total() == static_cast<int64_t>(rig.lib.searchRows("sargent", 0, 500).size()));
}

TEST_CASE("search screen: recent searches show before a search, run again on a tap, and clear") {
    Rig rig;
    rig.lib.rememberSearch("kuhn");
    rig.lib.rememberSearch("rawls");
    SearchScreen s([&](const ItemRow& r) { rig.opened.push_back(r.key); });
    s.onShow(rig.nav);
    REQUIRE(s.recentRects().size() == 2);
    CHECK(s.recent()[0] == "rawls");
    s.onGesture(tapAt(s.recentRects()[1].center()), rig.nav);  // kuhn
    CHECK(s.results().pager().total() > 0);
    CHECK(s.recentRects().empty());  // results now
    CHECK(rig.lib.recentSearches()[0] == "kuhn");
    // Opening a result remembers what found it.
    SearchScreen t([&](const ItemRow& r) { rig.opened.push_back(r.key); });
    t.onShow(rig.nav);
    rig.type(t, "rawls");
    rig.nav.now += SearchScreen::kPauseMs;
    t.onTick(rig.nav);
    rig.lib.clearRecentSearches();
    REQUIRE_FALSE(t.results().rows().empty());
    const Rect area = t.resultsArea(rig.nav);
    t.onGesture(tapAt(t.results().rowRects(area)[0].center()), rig.nav);
    REQUIRE(rig.opened.size() == 1);
    CHECK(rig.lib.recentSearches() == std::vector<std::string>{"rawls"});
    // Clear.
    SearchScreen u([](const ItemRow&) {});
    u.onShow(rig.nav);
    REQUIRE_FALSE(u.recentRects().empty());
    u.onGesture(tapAt(u.clearRect().center()), rig.nav);
    CHECK(u.recentRects().empty());
    CHECK(rig.lib.recentSearches().empty());
}

TEST_CASE("collection screen: the header's search icon opens a search in that collection") {
    Rig rig;
    CollectionEntry e;
    e.collection = {"COLLAAAA", "Economics", "", 1};
    CollectionScreen c(e, nullptr);
    c.onShow(rig.nav);
    c.onGesture(tapAt(CollectionScreen::searchRect().center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* s = dynamic_cast<SearchScreen*>(rig.nav.pushed[0].get());
    REQUIRE(s);
    CHECK(s->scopeKey() == "COLLAAAA");
}

TEST_CASE("search in a collection: recent searches and the scope chip match the reviewed goldens") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Rig rig;
    for (const char* q : {"coase", "nash equilibrium", "bellman"}) rig.lib.rememberSearch(q);
    SearchScreen s([](const ItemRow&) {}, {}, "COLLAAAA", "Economics");
    s.onShow(rig.nav);
    rig.nav.keyboardFake.hide();
    Canvas c(1236, 1547);
    s.draw(c);
    test::checkGolden(c, "tests/golden/search-recent.png");
    rig.type(s, "sargent");
    s.onKey({KeyEvent::Code::Return, ""}, rig.nav);
    s.draw(c);
    test::checkGolden(c, "tests/golden/search-in-collection.png");
    CollectionEntry e;
    e.collection = {"COLLAAAA", "Economics", "", 1};
    CollectionScreen col(e, nullptr);
    col.onShow(rig.nav);
    col.draw(c);
    test::checkGolden(c, "tests/golden/collection-search-icon.png");
}
