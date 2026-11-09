// The app's own confirmation of a plugin's removal: drawn by App over any screen, taking gestures
// first, shown after a page turn, one at a time, each answered once.

#include <doctest/doctest.h>

#include <memory>

#include "app/app.h"
#include "app/plugin_events.h"
#include "fake_display.h"
#include "fake_plugins.h"
#include "fake_sync.h"
#include "golden.h"
#include "plugins/host.h"
#include "screens/plugin_confirm.h"
#include "store/db.h"
#include "store/library.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

Point centre(Rect r) { return {r.x + r.w / 2, r.y + r.h / 2}; }

bool inside(Rect outer, Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

class NullKeyboard : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

// A reading screen whose page turn the test controls.
struct PageScreen : Screen {
    bool turningNow = false;
    int gestures = 0;
    void draw(Canvas& c) override {
        cairo_t* cr = c.cr();
        gray(cr, 255);
        cairo_paint(cr);
        drawText(cr, "Chapter 3. Gaussian random fields are random functions over a space whose finite "
                     "collections of values have joint Gaussian distributions.",
                 {theme::kSerif, 10, 400}, {kMargin, 120, c.width() - 2 * kMargin, 900}, 12);
    }
    void onGesture(const Gesture&, Nav&) override { ++gestures; }
    bool midTurn() const override { return turningNow; }
    bool showsTabBar() const override { return false; }
};

ItemRecord record(const std::string& key, const std::string& title) {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = "preprint";
    r.title = title;
    r.json = "{}";
    return r;
}

struct Rig {
    FakePlugins fp;
    std::string dbPath = fp.path("library.db");
    Db db = Db::open(dbPath);
    Library lib{db};
    FakeDisplay display;
    FakeSync sync;
    NullKeyboard keyboard;
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [this] { return now; }};
    PageScreen* page = nullptr;
    std::unique_ptr<PluginHost> host;
    std::unique_ptr<PluginEvents> events;

    Rig() {
        lib.upsertItem(record("AAAAAAA1", "Gaussian random fields (v1)"));
        lib.upsertItem(record("BBBBBBB1", "Attention is all you need"));
        fp.add("riffle", R"(
            zk.on("bookOpened", async (e) => {
              const r = await zk.zotero.trash(e.key);
              console.log("OUT " + e.key + " " + JSON.stringify(r));
            });
        )", {"library:read", "zotero:remove"}, "Feed Riffle");
        PluginHostDeps d = fp.deps();
        d.dbPath = dbPath;
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("riffle", true);
        host->start();
        events = std::make_unique<PluginEvents>(*host);
        app.setPollHook([this] { events->poll(app); });
        auto s = std::make_unique<PageScreen>();
        page = s.get();
        for (int i = 0; i < App::kTabs; ++i) app.setRoot(i, i == 0 ? std::move(s) : std::make_unique<PageScreen>());
        app.start();
    }
    void step() {
        app.step(0);
        now += 70;
    }
    // Steps the app until `cond` holds (the plugin thread answers meanwhile).
    bool until(const std::function<bool()>& cond) {
        return eventually([&] {
            step();
            return cond();
        });
    }
    void tap(Point p) {
        display.tap(p, now);
        step();
        step();
    }
    std::string log() const { return fp.log("riffle"); }
};

}  // namespace

TEST_CASE("confirm: the overlay's buttons and copy") {
    PluginConfirm c("Feed Riffle", {"Feed Riffle wants to move 'Gaussian random fields (v1)' to the Zotero trash."},
                    "Move to trash");
    CHECK(c.pluginName() == "Feed Riffle");
    CHECK(c.action() == "Move to trash");
    CHECK_FALSE(c.yesRect().empty());
    CHECK_FALSE(c.noRect().empty());
    CHECK(inside(c.rect(), c.yesRect()));
    CHECK(inside(c.rect(), c.noRect()));
    CHECK(c.onTap(centre(c.yesRect())) == std::optional<bool>(true));
    CHECK(c.onTap(centre(c.noRect())) == std::optional<bool>(false));
    CHECK_FALSE(c.onTap({c.rect().x + 10, c.rect().y + 10}).has_value());  // inside, not on a button
    CHECK_FALSE(c.onTap({5, 5}).has_value());                              // outside: it can't be dismissed
    // A long list stays on screen.
    std::vector<std::string> many{"Feed Riffle wants to move these items to the Zotero trash:"};
    for (int i = 0; i < 40; ++i) many.push_back("'Item number " + std::to_string(i) + "'");
    PluginConfirm list("Feed Riffle", many, "Move to trash");
    CHECK(list.rect().y >= 0);
    CHECK(list.rect().y + list.rect().h <= metrics().height);
}

TEST_CASE("confirm: [Review Focus] a Confirm arriving during a page turn is shown after it; a second one queues; each promise resolves once") {
    Rig rig;
    rig.page->turningNow = true;
    rig.host->dispatch({"bookOpened", {{"key", "AAAAAAA1"}}, true});
    rig.host->dispatch({"bookOpened", {{"key", "BBBBBBB1"}}, true});
    // Both questions reach the app, but the page is turning: nothing is shown over it.
    REQUIRE(eventually([&] {
        rig.step();
        return rig.events->waiting() == 2;
    }));
    for (int i = 0; i < 5; ++i) rig.step();
    CHECK(rig.app.confirmShown() == nullptr);

    rig.page->turningNow = false;
    rig.display.presents.clear();
    rig.step();
    const PluginConfirm* shown = rig.app.confirmShown();
    REQUIRE(shown);
    CHECK(shown->lines()[0] == "Feed Riffle wants to move 'Gaussian random fields (v1)' to the Zotero trash.");
    bool presented = false;
    for (const auto& p : rig.display.presents) presented = presented || inside(p.rect, shown->rect());
    CHECK(presented);
    const Rect yes = shown->yesRect(), no = shown->noRect();

    // It takes the gestures: a swipe or a tap elsewhere doesn't reach the page, nor answers.
    const int before = rig.page->gestures;
    rig.now += 1000;
    rig.tap({20, 20});
    CHECK(rig.page->gestures == before);
    REQUIRE(rig.app.confirmShown());

    rig.tap(centre(yes));
    CHECK(rig.app.confirmShown() == nullptr);  // the next waits for the next step at the earliest
    REQUIRE(rig.until([&] { return contains(rig.log(), "OUT AAAAAAA1"); }));
    REQUIRE(rig.until([&] { return rig.app.confirmShown() != nullptr; }));
    CHECK(rig.app.confirmShown()->lines()[0] == "Feed Riffle wants to move 'Attention is all you need' to the Zotero trash.");
    // A tap right as it appears (meant for the page) doesn't answer it.
    rig.tap(centre(no));
    REQUIRE(rig.app.confirmShown());
    rig.now += 1000;
    rig.tap(centre(no));
    CHECK(rig.app.confirmShown() == nullptr);
    REQUIRE(rig.until([&] { return contains(rig.log(), "OUT BBBBBBB1"); }));
    REQUIRE(rig.host->waitIdle(3000));
    for (int i = 0; i < 5; ++i) rig.step();

    const std::string log = rig.log();
    INFO(log);
    CHECK(count(log, "OUT AAAAAAA1 {") == 1);
    CHECK(count(log, "OUT AAAAAAA1") == 1);
    CHECK(count(log, "OUT BBBBBBB1 null") == 1);
    CHECK(count(log, "OUT BBBBBBB1") == 1);
    CHECK(rig.events->waiting() == 0);
    CHECK_FALSE(rig.lib.item("AAAAAAA1"));
    CHECK(rig.lib.item("BBBBBBB1"));
    CHECK(rig.sync.pushes > 0);  // the yes queued a change: the app pushes soon
}

TEST_CASE("confirm: a page turn that never lands holds a question back at most kTurnWaitMs") {
    Rig rig;
    rig.page->turningNow = true;
    rig.host->dispatch({"bookOpened", {{"key", "AAAAAAA1"}}, true});
    REQUIRE(eventually([&] {
        rig.step();
        return rig.events->waiting() == 1;
    }));
    const int64_t from = rig.now;
    REQUIRE(rig.until([&] { return rig.app.confirmShown() != nullptr; }));
    CHECK(rig.now - from >= App::kTurnWaitMs);
    CHECK(rig.now - from < App::kTurnWaitMs + 500);
}

TEST_CASE("confirm: a question withdrawn by the host goes away unanswered") {
    Rig rig;
    rig.host->dispatch({"bookOpened", {{"key", "AAAAAAA1"}}, true});
    REQUIRE(rig.until([&] { return rig.app.confirmShown() != nullptr; }));
    const Rect box = rig.app.confirmShown()->rect();
    rig.display.presents.clear();
    rig.host->setEnabled("riffle", false);  // turned off while it asks
    REQUIRE(rig.until([&] { return rig.app.confirmShown() == nullptr; }));
    bool repainted = false;
    for (const auto& p : rig.display.presents) repainted = repainted || inside(p.rect, box);
    CHECK(repainted);
    CHECK(rig.events->waiting() == 0);
    CHECK_FALSE(contains(rig.log(), "OUT AAAAAAA1"));
    CHECK(rig.lib.item("AAAAAAA1"));
}

TEST_CASE("confirm: golden") {
    addFontDir("assets/fonts-device");
    ScopedMetrics pw5(kPw5);
    Canvas c(1236, 1547);
    PageScreen page;
    SUBCASE("one item") {
        page.draw(c);
        PluginConfirm("Feed Riffle", {"Feed Riffle wants to move 'Gaussian random fields (v1)' to the Zotero trash."},
                      "Move to trash")
            .draw(c.cr());
        checkGolden(c, "tests/golden/plugin_confirm.png");
    }
    SUBCASE("several") {
        page.draw(c);
        PluginConfirm("Feed Riffle",
                      {"Feed Riffle wants to make these changes:", "Move 'Gaussian random fields (v1)' to the Zotero trash",
                       "Remove the tag 'to-read' from 'Attention is all you need'",
                       "Delete a note it added to 'A very long title of a book that goes on and on well past the edge of the box'"},
                      "Allow")
            .draw(c.cr());
        checkGolden(c, "tests/golden/plugin_confirm_list.png");
    }
}
