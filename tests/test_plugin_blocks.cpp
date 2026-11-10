// Plugin screen blocks: layout with paging, drawing in the app's style, hit-testing
// (src/screens/plugin_blocks.h). Goldens on PW5; KT4 and Scribe get fit assertions.

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <set>

#include "golden.h"
#include "screens/plugin_blocks.h"
#include "ui/canvas.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
using K = Block::Kind;

namespace {

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

Block tree(const json& j) {
    std::string why;
    auto t = parseTree(j, &why);
    INFO(why);
    REQUIRE(t);
    return *t;
}

const char* kSummary =
    "We construct Gaussian random fields on compact Riemannian manifolds whose covariance is a function of the "
    "Laplace-Beltrami operator, and study the regularity of their sample paths.\n\nAs an application we obtain sharp "
    "Hölder exponents for solutions of stochastic partial differential equations driven by such noise.";

json deckScreen(const std::string& body = kSummary) {
    json j = json::parse(R"({"kind":"column","children":[
      {"kind":"row","children":[{"kind":"text","text":"math.PR · 12 left","style":"label"},{"kind":"button","id":"feeds","title":"Feeds"}]},
      {"kind":"deck","id":"card","card":{"kind":"card","title":"Gaussian random fields on manifolds via the Laplace–Beltrami operator",
        "meta":"Adler, Taylor, Lang et al. · 2024-01-03","badge":"In library"}},
      {"kind":"text","text":"File into","style":"label"},
      {"kind":"chips","id":"recent","select":"none","chips":[{"id":"k1","title":"Probability / SPDEs"},{"id":"k2","title":"Reading"}]},
      {"kind":"buttons","buttons":[{"id":"discard","title":"Discard"},{"id":"skip","title":"Skip"},{"id":"keep","title":"Keep","primary":true}]},
      {"kind":"buttons","buttons":[{"id":"undo","title":"Undo"},{"id":"done","title":"Done"}]},
      {"kind":"notice","text":"Discarded “Brownian loops and the central charge”"}
    ]})");
    j["children"][1]["card"]["body"] = body;
    return j;
}

json fileScreen(int collections = 5) {
    json rows = json::array();
    const char* names[] = {"Probability / SPDEs", "Probability / Random fields", "Reading", "Geometry / Manifolds", "Teaching"};
    for (int i = 0; i < collections; ++i)
        rows.push_back({{"id", "c" + std::to_string(i)},
                        {"title", i < 5 ? std::string(names[i]) : "Collection " + std::to_string(i)},
                        {"selected", i == 0}});
    rows[1]["badge"] = "already in";
    json j = json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"Gaussian random fields on manifolds via the Laplace–Beltrami operator","style":"title","lines":2},
      {"kind":"notice","text":"Already in your library — filing adds a second item. The old one holds 2 notes."},
      {"kind":"chips","id":"replace","chips":[{"id":"trash","title":"Move the old one to the trash"}],"selected":[]},
      {"kind":"search","id":"find","placeholder":"Collection","focus":true},
      {"kind":"list","id":"collections","rows":[],"empty":"No collection matches"},
      {"kind":"text","text":"Tags","style":"label"},
      {"kind":"chips","id":"tags","chips":["to-read","spde","random-fields"],"selected":["to-read","spde"]},
      {"kind":"field","id":"tag","placeholder":"Add a tag","value":""},
      {"kind":"field","id":"note","label":"Note","lines":3},
      {"kind":"button","id":"file","title":"File into Probability / SPDEs","primary":true}
    ]})");
    j["children"][4]["rows"] = rows;
    return j;
}

json gallery() {
    return json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"Every block","style":"title"},
      {"kind":"text","text":["Body text with ",{"text":"bold","bold":true}," and ",{"text":"italic","italic":true}," stretches; it wraps at word boundaries like the rest of the app's text."]},
      {"kind":"text","text":"A label","style":"label"},
      {"kind":"card","title":"A card alone","meta":"Its meta line","body":"A short body.","badge":"New"},
      {"kind":"list","id":"l","rows":[{"id":"a","title":"All feeds","meta":"42","selected":true},{"id":"b","title":"math.PR","meta":"12","badge":"new"}]},
      {"kind":"row","children":[{"kind":"text","text":"A row: text and a button","style":"meta"},{"kind":"button","id":"go","title":"Go"}]},
      {"kind":"search","id":"s","placeholder":"Search…"},
      {"kind":"chips","id":"c","chips":["one","two","three"],"selected":["two"]},
      {"kind":"divider"},
      {"kind":"button","id":"b1","title":"A button"},
      {"kind":"space","size":"small"},
      {"kind":"notice","text":"A quiet notice."}
    ]})");
}

void collectRects(const Laid& l, std::vector<Rect>& out) {
    out.push_back(l.rect);
    for (const auto& p : l.parts) out.push_back(p.rect);
    for (const auto& c : l.children) collectRects(c, out);
}

bool inside(Rect r, Rect area) { return r.empty() || (r.x >= area.x && r.y >= area.y && r.x + r.w <= area.x + area.w && r.y + r.h <= area.y + area.h); }

// Nothing outside the area, in the layout or in pixels, on any page; and every unit of every
// top-level block shown on some page.
void checkFits(const Block& root) {
    const Rect area = blocksArea();
    const Laid first = layoutTree(root, area, 0);
    std::map<const Block*, std::set<int>> shown;
    std::set<const Block*> seen;
    Canvas c(metrics().width, metrics().height);
    for (int p = 0; p < first.pages; ++p) {
        const Laid l = layoutTree(root, area, p);
        CHECK(l.page == p);
        CHECK(l.pages == first.pages);
        std::vector<Rect> rects;
        collectRects(l, rects);
        for (Rect r : rects) {
            INFO(r.x << "," << r.y << " " << r.w << "x" << r.h << " page " << p);
            CHECK(inside(r, area));
        }
        for (const auto& k : l.children) {
            seen.insert(k.block);
            for (int u = k.first; u < k.first + k.count; ++u) shown[k.block].insert(u);
        }
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 128);
        drawTree(c.cr(), l, {});
        const auto g = grayOf(c);
        size_t outside = 0;
        for (int y = 0; y < c.height(); ++y)
            for (int x = 0; x < c.width(); ++x)
                if (!area.contains({x, y}) && g[(size_t)y * c.width() + x] != 128) ++outside;
        CHECK(outside == 0);
    }
    REQUIRE(root.kind == K::Column);
    for (const auto& k : root.children) {
        CHECK(seen.count(&k));
        // count the units the block has: the most any page showed when laid alone, tall
        const Laid alone = layoutTree(k, {0, 0, area.w, 1 << 28}, 0);
        REQUIRE(alone.children.size() == 1);
        const int units = alone.children[0].count;
        CHECK(static_cast<int>(shown[&k].size()) == units);
    }
}

}  // namespace

TEST_CASE("plugin blocks: goldens on PW5 — every block, the deck, the filing screen") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Canvas c(kPw5.width, kPw5.height);
    const auto shot = [&](const Block& t, const char* title, const char* path, int page = 0, FocusState f = {}) {
        fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255);
        drawHeader(c.cr(), title, true);
        drawTree(c.cr(), layoutTree(t, blocksArea(), page), f);
        checkGolden(c, path);
    };
    shot(tree(gallery()), "Every block", "tests/golden/plugin-blocks-gallery.png");
    shot(tree(deckScreen()), "Feed Riffle", "tests/golden/plugin-blocks-deck.png");
    FocusState typing;
    typing.focused = "find";
    typing.texts["find"] = "prob";
    shot(tree(fileScreen()), "Keep", "tests/golden/plugin-blocks-file.png", 0, typing);
    shot(tree(json::parse(R"({"kind":"deck","id":"d","empty":"Nothing left to riffle"})")), "Feed Riffle",
         "tests/golden/plugin-blocks-empty-deck.png");
    std::string longBody;
    for (int i = 0; i < 40; ++i) longBody += std::string(kSummary) + (i % 3 == 2 ? "\n\n" : " ");
    shot(tree(deckScreen(longBody)), "Feed Riffle", "tests/golden/plugin-blocks-deck-page2.png", 1);
}

TEST_CASE("plugin blocks: everything fits and is reachable on PW5, KT4 and Scribe") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        checkFits(tree(gallery()));
        checkFits(tree(deckScreen()));
        checkFits(tree(fileScreen()));
        checkFits(tree(fileScreen(40)));
    }
}

TEST_CASE("plugin blocks: a 10,000-character card body pages with the buttons kept on KT4") {
    if (!fonts()) return;
    ScopedMetrics m(kKt4);
    std::string body;
    while (body.size() < 10000) body += std::string(kSummary) + "\n\n";
    const Block t = tree(deckScreen(body));
    const Laid l0 = layoutTree(t, blocksArea(), 0);
    CHECK(l0.pages > 3);
    for (int p = 0; p < l0.pages; ++p) {
        const Laid l = layoutTree(t, blocksArea(), p);
        REQUIRE(l.children.size() == 7);  // elastic: every block on every page
        CHECK(hitTest(l, l.children[4].parts[2].rect.center())->id == "keep");
    }
    checkFits(t);
    // Past the end: the last page.
    CHECK(layoutTree(t, blocksArea(), 999).page == l0.pages - 1);
}

TEST_CASE("plugin blocks: a 300-row list pages; rows hit-test to their ids") {
    if (!fonts()) return;
    ScopedMetrics m(kKt4);
    json rows = json::array();
    for (int i = 0; i < 300; ++i) rows.push_back({{"id", "r" + std::to_string(i)}, {"title", "Row " + std::to_string(i)}});
    json j = json::parse(R"({"kind":"column","children":[{"kind":"list","id":"rows","rows":[]},
      {"kind":"button","id":"fetch","title":"Fetch now"}]})");
    j["children"][0]["rows"] = rows;
    const Block t = tree(j);
    const Laid l = layoutTree(t, blocksArea(), 2);
    CHECK(l.pages > 10);
    const Laid& list = l.children[0];
    REQUIRE(list.count > 0);
    const auto hit = hitTest(l, list.parts[1].rect.center());
    REQUIRE(hit);
    CHECK(hit->id == "rows");
    CHECK(hit->part == "r" + std::to_string(list.first + 1));
    CHECK(hitTest(l, l.children[1].rect.center())->id == "fetch");
    CHECK(pageIndicatorRect(l).y + pageIndicatorRect(l).h <= blocksArea().y + blocksArea().h);
    checkFits(t);
}

TEST_CASE("plugin blocks: hit-testing buttons, chips, search and the deck") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    const Block t = tree(deckScreen());
    const Laid l = layoutTree(t, blocksArea(), 0);
    REQUIRE(l.pages == 1);
    CHECK(hitTest(l, l.children[0].children[1].rect.center())->id == "feeds");
    CHECK(hitTest(l, l.children[1].rect.center())->id == "card");
    CHECK(hitTest(l, l.children[1].rect.center())->kind == K::Deck);
    const auto chip = hitTest(l, l.children[3].parts[1].rect.center());
    REQUIRE(chip);
    CHECK(chip->id == "recent");
    CHECK(chip->part == "k2");
    CHECK(hitTest(l, l.children[4].parts[0].rect.center())->id == "discard");
    CHECK(hitTest(l, l.children[5].parts[1].rect.center())->id == "done");
    CHECK_FALSE(hitTest(l, l.children[6].rect.center()));  // a notice
    const Block f = tree(fileScreen());
    const Laid lf = layoutTree(f, blocksArea(), 0);
    REQUIRE(findLaid(lf, "find"));
    CHECK(hitTest(lf, findLaid(lf, "find")->rect.center())->kind == K::Search);
    CHECK(findLaid(lf, "nope") == nullptr);
}
