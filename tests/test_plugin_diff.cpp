// Redrawing only what a plugin changed (src/screens/plugin_diff.h).

#include <doctest/doctest.h>

#include <filesystem>

#include "screens/plugin_diff.h"
#include "ui/metrics.h"
#include "ui/text.h"

using namespace zk;
using json = nlohmann::json;

namespace {

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

Block tree(const std::string& s) {
    std::string why;
    auto t = parseTree(json::parse(s), &why);
    INFO(why);
    REQUIRE(t);
    return *t;
}

const char* kDeck = R"({"kind":"column","children":[
  {"kind":"text","text":"math.PR · 12 left","style":"label"},
  {"kind":"deck","id":"card","card":{"kind":"card","title":"First card","meta":"Adler","body":"Short."}},
  {"kind":"buttons","buttons":[{"id":"discard","title":"Discard"},{"id":"keep","title":"Keep","primary":true}]},
  {"kind":"notice","text":"Kept"}
]})";

std::string with(std::string s, const std::string& from, const std::string& to) {
    s.replace(s.find(from), from.size(), to);
    return s;
}

}  // namespace

TEST_CASE("plugin diff: identical trees need no redraw") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(kDeck);
    CHECK(diffTrees(layoutTree(a, blocksArea(), 0), layoutTree(b, blocksArea(), 0)).empty());
}

TEST_CASE("plugin diff: a button's label redraws that block with Ui; a notice's text with Fast") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(with(kDeck, "\"Discard\"", "\"Drop\""));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lb.children[2].rect);
    CHECK(d[0].wave == Waveform::Ui);

    const Block c = tree(with(kDeck, "\"Kept\"", "\"Discarded\""));
    d = diffTrees(la, layoutTree(c, blocksArea(), 0));
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == la.children[3].rect);
    CHECK(d[0].wave == Waveform::Fast);
}

TEST_CASE("plugin diff: another card in the deck redraws the deck with Quality") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(with(kDeck, "First card", "Second"));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    const auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == la.children[1].rect);
    CHECK(d[0].wave == Waveform::Quality);
}

TEST_CASE("plugin diff: a list's page turn redraws the list and the indicator with Quality") {
    if (!fonts()) return;
    ScopedMetrics m(kKt4);
    json rows = json::array();
    for (int i = 0; i < 50; ++i) rows.push_back({{"id", std::to_string(i)}, {"title", "Row " + std::to_string(i)}});
    json j = json::parse(R"({"kind":"column","children":[{"kind":"list","id":"l","rows":[]},{"kind":"button","id":"b","title":"Fetch now"}]})");
    j["children"][0]["rows"] = rows;
    const Block t = tree(j.dump());
    const Laid p0 = layoutTree(t, blocksArea(), 0), p1 = layoutTree(t, blocksArea(), 1);
    REQUIRE(p0.pages > 1);
    const auto d = diffTrees(p0, p1);
    REQUIRE(d.size() == 2);  // the list; the indicator (the button between them is unchanged)
    CHECK(d[0].rect == p0.children[0].rect);
    CHECK(d[0].wave == Waveform::Quality);
    CHECK(d[1].rect == pageIndicatorRect(p1));
    CHECK(d[1].wave == Waveform::Quality);
}

TEST_CASE("plugin diff: a block inserted above redraws the region that moved") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block a = tree(kDeck), b = tree(with(kDeck, R"({"kind":"deck")", R"({"kind":"notice","text":"New cards"},{"kind":"deck")"));
    const Laid la = layoutTree(a, blocksArea(), 0), lb = layoutTree(b, blocksArea(), 0);
    const auto d = diffTrees(la, lb);
    REQUIRE(d.size() == 1);
    CHECK(d[0].rect == lb.children[1].rect.united(lb.children.back().rect).united(la.children[1].rect));
    CHECK(d[0].rect.y == la.children[1].rect.y);  // the label above is not redrawn
    CHECK(d[0].wave == Waveform::Ui);  // blocks moved, nothing turned
}
