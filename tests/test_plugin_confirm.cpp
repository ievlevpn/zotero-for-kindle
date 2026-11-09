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
    bool wantsInk = false;  // like the reader on a PDF page: asks for the system's wet ink every tick
    int pens = 0;
    void onGesture(const Gesture&, Nav&) override { ++gestures; }
    bool onPen(const PenEvent&, Nav&) override {
        ++pens;
        return false;  // a finger, like a screen that doesn't write
    }
    void onTick(Nav& nav) override {
        if (wantsInk && nav.systemInk()) nav.setSystemInk(true);
    }
    bool wantsSystemInk() const override { return wantsInk; }
    bool midTurn() const override { return turningNow; }
    bool showsTabBar() const override { return false; }
};

// A PDF page that writes and erases with the pen, as the reader does: a stroke ends (committed, or
// its eraser path applied) when the screen gets its lift.
struct InkScreen : PageScreen {
    bool inkDown = false, erasing = false;
    std::vector<Point> current;
    std::vector<std::vector<Point>> strokes, erased;
    bool onPen(const PenEvent& p, Nav&) override {
        ++pens;
        if (p.type == PenEvent::Type::Up || p.type == PenEvent::Type::Leave) {
            if (inkDown) strokes.push_back(current);
            if (erasing) erased.push_back(current);
            inkDown = erasing = false;
            current.clear();
            return false;
        }
        if (p.type == PenEvent::Type::Near) return false;
        if (p.type == PenEvent::Type::Down) {
            (p.eraser ? erasing : inkDown) = true;
            current = {p.pos};
            return true;
        }
        if (!inkDown && !erasing) return false;  // a stroke it never saw go down
        current.push_back(p.pos);
        return true;
    }
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
    InkScreen* ink = nullptr;
    std::unique_ptr<PluginHost> host;
    std::unique_ptr<PluginEvents> events;

    explicit Rig(bool inkScreen = false) {
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
        events = std::make_unique<PluginEvents>(host.get());
        app.setPollHook([this] { events->poll(app); });
        std::unique_ptr<PageScreen> s;
        if (inkScreen) {
            auto i = std::make_unique<InkScreen>();
            ink = i.get();
            s = std::move(i);
        } else {
            s = std::make_unique<PageScreen>();
        }
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
    void pen(PenEvent::Type type, Point p, bool eraser = false) {
        display.pen(type, p, now, eraser);
        step();
    }
    void stroke(Point from, bool eraser = false) {
        pen(PenEvent::Type::Near, from, eraser);
        pen(PenEvent::Type::Down, from, eraser);
        pen(PenEvent::Type::Move, {from.x + 60, from.y}, eraser);
        pen(PenEvent::Type::Up, {from.x + 60, from.y}, eraser);
        pen(PenEvent::Type::Leave, {from.x + 60, from.y}, eraser);
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
    std::vector<std::string> many{"Feed Riffle wants to move these 40 items to the Zotero trash:"};
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

// The rig with one question shown and its tap guard over.
const PluginConfirm& showOne(Rig& rig) {
    rig.host->dispatch({"bookOpened", {{"key", "AAAAAAA1"}}, true});
    REQUIRE(rig.until([&] { return rig.app.confirmShown() != nullptr && rig.app.confirmShownSince() >= 0; }));
    rig.now += 1000;
    return *rig.app.confirmShown();
}

TEST_CASE("confirm: the pen never answers it, and no wet ink shows while it does") {
    Rig rig;
    rig.display.hasInk = true;
    rig.page->wantsInk = true;
    rig.step();
    rig.step();
    REQUIRE(rig.display.inkOn);  // the page had it on
    const PluginConfirm& box = showOne(rig);
    const Rect yes = box.yesRect();
    for (int i = 0; i < 5; ++i) rig.step();
    CHECK_FALSE(rig.display.inkOn);  // off, and the page's ticks don't turn it back on
    const int pens = rig.page->pens;
    // A short pen dot on the action button.
    rig.display.pen(PenEvent::Type::Near, centre(yes), rig.now);
    rig.display.pen(PenEvent::Type::Down, centre(yes), rig.now + 10);
    rig.display.pen(PenEvent::Type::Up, centre(yes), rig.now + 40);
    rig.display.pen(PenEvent::Type::Leave, centre(yes), rig.now + 50);
    for (int i = 0; i < 6; ++i) rig.step();
    CHECK(rig.app.confirmShown() != nullptr);
    CHECK(rig.page->pens == pens);  // nor written under it
    CHECK_FALSE(rig.display.inkOn);
    // A finger does answer.
    rig.now += 1000;  // past the palm rejection after the pen left
    rig.tap(centre(yes));
    CHECK(rig.app.confirmShown() == nullptr);
    REQUIRE(rig.until([&] { return contains(rig.log(), "OUT AAAAAAA1 {"); }));
    for (int i = 0; i < 3; ++i) rig.step();
    CHECK(rig.display.inkOn);  // the page may have it again
}

// Answers the shown question with a finger (Cancel), once the pen is away and the guard is over.
void answerNo(Rig& rig) {
    REQUIRE(rig.app.confirmShown());
    rig.now += 1500;
    rig.tap(centre(rig.app.confirmShown()->noRect()));
    REQUIRE(rig.app.confirmShown() == nullptr);
}

TEST_CASE("confirm: a question waits for the pen to lift; the stroke is kept and the next one draws") {
    for (const bool eraser : {false, true}) {
        CAPTURE(eraser);
        Rig rig(true);
        rig.pen(PenEvent::Type::Near, {300, 900}, eraser);
        rig.pen(PenEvent::Type::Down, {300, 900}, eraser);
        rig.pen(PenEvent::Type::Move, {340, 900}, eraser);
        rig.host->dispatch({"bookOpened", {{"key", "AAAAAAA1"}}, true});
        REQUIRE(eventually([&] {
            rig.step();
            return rig.events->waiting() == 1;
        }));
        for (int i = 0; i < 5; ++i) rig.step();
        CHECK(rig.app.confirmShown() == nullptr);  // the pen is on the glass
        rig.pen(PenEvent::Type::Move, {380, 900}, eraser);
        rig.pen(PenEvent::Type::Up, {380, 900}, eraser);
        rig.pen(PenEvent::Type::Leave, {380, 900}, eraser);
        rig.step();
        REQUIRE(rig.app.confirmShown() != nullptr);
        CHECK(rig.ink->strokes.size() == (eraser ? 0u : 1u));
        CHECK(rig.ink->erased.size() == (eraser ? 1u : 0u));
        const auto& ended = eraser ? rig.ink->erased : rig.ink->strokes;
        CHECK((!ended.empty() && ended.back().size() == 3));  // all of it
        answerNo(rig);
        rig.stroke({300, 1100});  // the next stroke writes
        CHECK(rig.ink->strokes.size() == (eraser ? 1u : 2u));
        CHECK(rig.ink->erased.size() == (eraser ? 1u : 0u));  // and erases nothing
    }
}

TEST_CASE("confirm: a stroke still open when the wait runs out ends there; the next one draws, not erases") {
    for (const bool eraser : {false, true}) {
        CAPTURE(eraser);
        Rig rig(true);
        rig.pen(PenEvent::Type::Near, {300, 900}, eraser);
        rig.pen(PenEvent::Type::Down, {300, 900}, eraser);
        rig.pen(PenEvent::Type::Move, {340, 900}, eraser);
        rig.host->dispatch({"bookOpened", {{"key", "AAAAAAA1"}}, true});
        REQUIRE(eventually([&] {
            rig.step();
            return rig.events->waiting() == 1;
        }));
        const int64_t from = rig.now;
        // The pen stays down (still reporting) past the wait.
        while (rig.app.confirmShown() == nullptr && rig.now - from < App::kTurnWaitMs + 1000)
            rig.pen(PenEvent::Type::Move, {340, 900}, eraser);
        REQUIRE(rig.app.confirmShown() != nullptr);
        CHECK(rig.now - from >= App::kTurnWaitMs);
        // The screen got its lift: the stroke ended as drawn so far.
        CHECK_FALSE(rig.ink->inkDown);
        CHECK_FALSE(rig.ink->erasing);
        CHECK(rig.ink->strokes.size() == (eraser ? 0u : 1u));
        CHECK(rig.ink->erased.size() == (eraser ? 1u : 0u));
        // The rest of that stroke goes nowhere.
        const int pens = rig.ink->pens;
        rig.pen(PenEvent::Type::Move, {420, 900}, eraser);
        rig.pen(PenEvent::Type::Up, {420, 900}, eraser);
        rig.pen(PenEvent::Type::Leave, {420, 900}, eraser);
        CHECK(rig.ink->pens == pens);
        answerNo(rig);
        rig.stroke({300, 1100});  // the pen's next stroke writes ink
        CHECK(rig.ink->strokes.size() == (eraser ? 1u : 2u));
        CHECK(rig.ink->erased.size() == (eraser ? 1u : 0u));
    }
}

TEST_CASE("confirm: a tap that went down before the guard ran out doesn't answer, however late it is handled") {
    Rig rig;
    rig.host->dispatch({"bookOpened", {{"key", "AAAAAAA1"}}, true});
    REQUIRE(rig.until([&] { return rig.app.confirmShown() != nullptr && rig.app.confirmShownSince() >= 0; }));
    const int64_t since = rig.app.confirmShownSince();
    const Rect yes = rig.app.confirmShown()->yesRect();
    // Went down 300 ms after the box was presented; the app gets to it at 520 ms, past the guard
    // by its own clock (and soon enough not to be a long press).
    rig.display.touch(TouchEvent::Type::Down, centre(yes), since + 300);
    rig.display.touch(TouchEvent::Type::Up, centre(yes), since + 360);
    rig.now = since + 520;
    rig.step();
    rig.step();
    CHECK(rig.app.confirmShown() != nullptr);
    // One that went down once the guard ran out answers.
    rig.display.touch(TouchEvent::Type::Down, centre(yes), since + App::kConfirmGuardMs + 200);
    rig.display.touch(TouchEvent::Type::Up, centre(yes), since + App::kConfirmGuardMs + 260);
    rig.step();
    rig.step();
    CHECK(rig.app.confirmShown() == nullptr);
}

TEST_CASE("confirm: Cancel is on the left, the action on the right") {
    PluginConfirm c("Feed Riffle", {"Feed Riffle wants to move 'X' to the Zotero trash."}, "Move to trash");
    CHECK(c.noRect().x < c.yesRect().x);
    CHECK(c.noRect().x + c.noRect().w < metrics().width / 2);
}

TEST_CASE("confirm: a host stopped for an install takes its questions away; a new host over the same folder asks again") {
    Rig rig;
    showOne(rig);
    rig.host->stop();
    rig.events->rebind(nullptr, rig.app);
    rig.step();
    CHECK(rig.app.confirmShown() == nullptr);
    CHECK(rig.events->waiting() == 0);
    rig.events->poll(rig.app);  // no host: nothing

    PluginHostDeps d = rig.fp.deps();
    d.dbPath = rig.dbPath;
    rig.host = std::make_unique<PluginHost>(d);
    rig.events->rebind(rig.host.get(), rig.app);
    rig.host->start();
    const Rect yes = showOne(rig).yesRect();
    rig.tap(centre(yes));
    REQUIRE(rig.until([&] { return contains(rig.log(), "OUT AAAAAAA1 {"); }));
    CHECK_FALSE(rig.lib.item("AAAAAAA1"));
    CHECK(count(rig.log(), "OUT AAAAAAA1") == 1);  // the first question was never answered
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
                      {"Feed Riffle wants to make these 3 changes:", "Move 'Gaussian random fields (v1)' to the Zotero trash",
                       "Remove the tag 'to-read' from 'Attention is all you need'",
                       "Delete a note it added to 'A very long title of a book that goes on and on well past the edge of the box'"},
                      "Allow")
            .draw(c.cr());
        checkGolden(c, "tests/golden/plugin_confirm_list.png");
    }
}
