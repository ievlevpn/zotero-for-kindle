// Where plugins appear: the reader's selection bar (More…), an item's details page (Plugins), Home's
// puzzle-piece icon and the Plugins launcher; running an action ("Working…", then the result
// pop-up: paged text, Close, the plugin's buttons re-running with {action, text}; errors; Close
// stops a run). Then PluginEvents over a real host, end to end.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <thread>

#include "app/app.h"
#include "app/plugin_events.h"
#include "docs/render_worker.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fake_sync.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/annotation_bar.h"
#include "screens/home.h"
#include "screens/item_details.h"
#include "screens/plugin_launcher.h"
#include "screens/plugin_result.h"
#include "screens/plugin_screen.h"
#include "screens/reader.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

const char* kLong =
    "Gaussian random fields with Matérn covariance are the solutions of a stochastic partial differential equation, "
    "which lets a sparse precision matrix stand in for a dense covariance.\n\n"
    "This is why the method scales: the Gaussian Markov random field on a triangulation has a precision matrix with "
    "only a few non-zeros per row, so factorising it costs little more than the number of nodes.\n\n"
    "In the paper the authors show the construction for the plane and the sphere, and how it extends to "
    "non-stationary fields by letting the parameters vary in space.\n\n"
    "A last paragraph so that the answer runs onto a second page of the pop-up on the Kindle, where the sheet takes "
    "a little more than half of the page and no more.";

struct FakeEntries : PluginEntries {
    std::vector<PluginAction> selection, item;
    std::vector<PluginLaunch> launch;
    bool badge = false;
    uint64_t v = 1;
    struct Run {
        PluginAction action;
        std::string kind;
        nlohmann::ordered_json target;
        json again;
    };
    std::map<uint64_t, Run> runs;
    std::map<uint64_t, ActionOutcome> outcomes;
    std::vector<uint64_t> stops;
    std::vector<std::pair<std::string, std::string>> opened;
    uint64_t next = 0;

    std::vector<PluginAction> actions(const std::string& kind) override { return kind == "itemAction" ? item : selection; }
    std::vector<PluginLaunch> mainScreens() override { return launch; }
    bool anyBadge() override { return badge; }
    uint64_t version() override { return v; }
    uint64_t run(const PluginAction& a, const std::string& kind, nlohmann::ordered_json target, json again) override {
        runs[++next] = {a, kind, std::move(target), std::move(again)};
        return next;
    }
    std::optional<ActionOutcome> take(uint64_t run) override {
        auto it = outcomes.find(run);
        if (it == outcomes.end()) return std::nullopt;
        ActionOutcome o = it->second;
        outcomes.erase(it);
        return o;
    }
    void stop(uint64_t run) override { stops.push_back(run); }
    void openScreen(const std::string& p, const std::string& s) override { opened.emplace_back(p, s); }
    void answer(std::string text, std::vector<std::pair<std::string, std::string>> buttons = {}) {
        outcomes[next] = ActionOutcome{false, false, std::move(text), std::move(buttons)};
    }
};

FakeEntries withActions() {
    FakeEntries e;
    e.selection = {{"explain", "Explain with AI", "explain", "Explain"}, {"deepl", "DeepL", "translate", "Translate"}};
    e.item = {{"md", "Highlights to Markdown", "export", "Export highlights"}};
    return e;
}

// ---- The reader's selection bar ---------------------------------------------------------------

struct ReaderRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;

    explicit ReaderRig(PluginEntries* entries, DocType type = DocType::Epub) {
        addFontDir("assets/fonts-device");
        nav.entries = entries;
        fillFixtureLibrary(lib, 10);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), type == DocType::Pdf ? "book.pdf" : "book.epub", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitFor([&] { return reader->ready(); }));
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void gesture(GestureType t, Point p, Point start) { reader->onGesture(Gesture{t, p, start}, nav); }
    void select() {
        reader->turnTo(99);  // a light page of the fake's
        REQUIRE(waitFor([&] { return reader->page() == 99; }));
        gesture(GestureType::LongPress, {130, 210}, {130, 210});
        gesture(GestureType::DragStart, {200, 215}, {130, 210});
        gesture(GestureType::DragMove, {650, 330}, {130, 210});
        gesture(GestureType::DragEnd, {650, 330}, {130, 210});
        REQUIRE(waitFor([&] { return reader->annotationBar() && reader->selection()->rects.size() == 3; }));
    }
    void tap(Rect r) { reader->onGesture(tapAt({r.x + r.w / 2, r.y + r.h / 2}), nav); }
    void shot(const char* path) {
        Canvas c(1236, 1547);
        reader->draw(c);
        checkGolden(c, path);
    }
};

}  // namespace

TEST_CASE("plugin entries: no selection action → the bar has its three buttons and matches its golden") {
    FakeEntries none;
    ReaderRig r(&none);
    r.select();
    AnnotationBar* bar = r.reader->annotationBar();
    CHECK(bar->hit({bar->buttonRect(2).x + 10, bar->buttonRect(2).y + 10}).action == AnnotationBar::Action::Note);
    CHECK(bar->buttonRect(2).x + bar->buttonRect(2).w > bar->rect().x + bar->rect().w - 40);  // the last of three
    r.shot("tests/golden/annotation-bar.png");
}

TEST_CASE("plugin entries: More… lists the selection actions; one runs on the selection, Working…, then the pop-up") {
    FakeEntries e = withActions();
    ReaderRig r(&e);
    r.select();
    AnnotationBar* bar = r.reader->annotationBar();
    REQUIRE(bar->hit({bar->buttonRect(3).x + 10, bar->buttonRect(3).y + 10}).action == AnnotationBar::Action::More);
    bar->setWorking(true);
    r.shot("tests/golden/plugin-more-bar.png");  // More… as it reads while an action runs
    bar->setWorking(false);
    r.tap(bar->buttonRect(3));
    REQUIRE(bar->listing());
    r.shot("tests/golden/plugin-more-list.png");
    const Rect explain = bar->actionRect(0);
    CHECK(bar->rect().contains({explain.x, explain.y}));
    r.tap(explain);
    REQUIRE(e.runs.size() == 1);
    const auto& run = e.runs[1];
    CHECK(run.kind == "selectionAction");
    CHECK(run.action.id == "explain");
    CHECK(run.target["text"].is_string());
    CHECK_FALSE(run.target["text"].get<std::string>().empty());
    CHECK(run.target["item"]["key"] == "IT000003");
    CHECK(run.target["attachment"]["key"] == "EP000003");
    CHECK(run.target.contains("pageLabel"));
    CHECK(run.again.is_null());
    CHECK_FALSE(bar->listing());
    CHECK(bar->working());  // More… says "Working…"
    REQUIRE(r.reader->pluginResult());
    CHECK_FALSE(r.reader->pluginResult()->shown());

    e.answer(kLong, {{"note", "Save as note"}, {"copy", "Keep"}});
    r.reader->onTick(r.nav);
    const PluginResult* p = r.reader->pluginResult();
    REQUIRE(p);
    CHECK(p->shown());
    CHECK_FALSE(r.reader->selection());  // the pop-up replaces the selection
    CHECK(p->pages() == 2);
    const Rect page = ReaderScreen::pageArea();
    CHECK(p->rect().y + p->rect().h == page.y + page.h);
    CHECK(p->rect().h <= page.h * 11 / 20);
    r.shot("tests/golden/plugin-result.png");
    r.reader->onGesture(swipe(Direction::Left), r.nav);
    CHECK(p->page() == 1);
    r.reader->onGesture(swipe(Direction::Left), r.nav);  // a pop-up swipe never turns the book's page
    CHECK(r.reader->page() == 99);

    r.tap(p->buttonRect(0));  // Save as note: run again with {action, text}
    REQUIRE(e.runs.size() == 2);
    CHECK(e.runs[2].again == json{{"action", "note"}, {"text", kLong}});
    CHECK(e.runs[2].target == run.target);
    CHECK(p->running());
    CHECK(p->buttonRect(0).empty());  // only Close while it works
    r.tap(p->closeRect());  // closing early stops the call
    CHECK(e.stops == std::vector<uint64_t>{2});
    CHECK_FALSE(r.reader->pluginResult());
}

TEST_CASE("plugin entries: an error shows in the pop-up; nothing returned shows nothing; a tap off the bar stops the run") {
    FakeEntries e = withActions();
    ReaderRig r(&e);
    SUBCASE("error") {
        r.select();
        r.tap(r.reader->annotationBar()->buttonRect(3));
        r.tap(r.reader->annotationBar()->actionRect(1));
        e.outcomes[1] = ActionOutcome{true, false, "DeepL couldn't do that: no network", {}};
        r.reader->onTick(r.nav);
        REQUIRE(r.reader->pluginResult());
        CHECK(r.reader->pluginResult()->text() == "DeepL couldn't do that: no network");
        CHECK(r.reader->pluginResult()->buttons().empty());
        r.tap(r.reader->pluginResult()->closeRect());
        CHECK_FALSE(r.reader->pluginResult());
        CHECK(e.stops.empty());  // nothing was running
    }
    SUBCASE("nothing") {
        r.select();
        r.tap(r.reader->annotationBar()->buttonRect(3));
        r.tap(r.reader->annotationBar()->actionRect(0));
        e.outcomes[1] = ActionOutcome{false, true, "", {}};
        r.reader->onTick(r.nav);
        CHECK_FALSE(r.reader->pluginResult());
        REQUIRE(r.reader->annotationBar());
        CHECK_FALSE(r.reader->annotationBar()->working());
    }
    SUBCASE("a tap off the bar") {
        r.select();
        r.tap(r.reader->annotationBar()->buttonRect(3));
        r.tap(r.reader->annotationBar()->actionRect(0));
        r.reader->onGesture(tapAt({600, 1300}), r.nav);
        CHECK_FALSE(r.reader->selection());
        CHECK_FALSE(r.reader->pluginResult());
        CHECK(e.stops == std::vector<uint64_t>{1});
    }
}

// ---- The item's details page --------------------------------------------------------------------

TEST_CASE("plugin entries: item details has a Plugins section; a row runs on the item; Working…; the pop-up; leaving stops") {
    if (!fonts()) return;
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    FakeEntries e = withActions();
    SUBCASE("none: no section") {
        FakeEntries none;
        nav.entries = &none;
        ItemDetailsScreen s(*lib.rowFor("IT000003"), nullptr);
        s.onShow(nav);
        Canvas c(1236, 1547);
        s.draw(c);
        CHECK(s.actionRects().empty());
    }
    nav.entries = &e;
    ItemDetailsScreen s(*lib.rowFor("IT000003"), [](const ItemRow&) {});
    s.onShow(nav);
    Canvas c(1236, 1547);
    s.draw(c);
    REQUIRE(s.actionRects().size() == 1);
    checkGolden(c, "tests/golden/item-details-plugins.png");
    const Rect row = s.actionRects()[0];
    s.onGesture(tapAt({row.x + 20, row.y + 20}), nav);
    REQUIRE(e.runs.size() == 1);
    CHECK(e.runs[1].kind == "itemAction");
    CHECK(e.runs[1].target["key"] == "IT000003");
    CHECK(e.runs[1].target.contains("title"));
    CHECK(e.runs[1].target.contains("tags"));
    REQUIRE(s.result());
    CHECK(s.result()->running());
    SUBCASE("the answer") {
        e.answer("Written to austen1813.md");
        s.onTick(nav);
        REQUIRE(s.result());
        CHECK(s.result()->shown());
        CHECK(s.result()->rect().y + s.result()->rect().h == metrics().height);
        s.onGesture(tapAt({600, 300}), nav);  // the pop-up takes every gesture
        CHECK(e.runs.size() == 1);
        const Rect close = s.result()->closeRect();
        s.onGesture(tapAt({close.x + 10, close.y + 10}), nav);
        CHECK_FALSE(s.result());
    }
    SUBCASE("leaving") {
        s.onClose(nav);
        CHECK(e.stops == std::vector<uint64_t>{1});
    }
    SUBCASE("the Working… row tapped again stops the run") {
        s.onGesture(tapAt({row.x + 20, row.y + 20}), nav);
        CHECK(e.stops == std::vector<uint64_t>{1});
        CHECK(e.runs.size() == 1);  // not a second run
        CHECK_FALSE(s.result());
    }
}

TEST_CASE("plugin entries: a row runs the action the list showed, even if plugins loaded or went since") {
    FakeEntries e = withActions();
    e.selection = {{"deepl", "DeepL", "translate", "Translate"}};
    ReaderRig r(&e);
    r.select();
    r.tap(r.reader->annotationBar()->buttonRect(3));
    REQUIRE(r.reader->annotationBar()->listing());
    SUBCASE("another plugin loaded meanwhile, sorted first") {
        e.selection.insert(e.selection.begin(), PluginAction{"explain", "Explain with AI", "explain", "Explain"});
        r.tap(r.reader->annotationBar()->actionRect(0));
        REQUIRE(e.runs.size() == 1);
        CHECK(e.runs[1].action.pluginId == "deepl");
        CHECK(e.runs[1].action.id == "translate");
    }
    SUBCASE("the picked one went (turned off, restarting)") {
        e.selection = {{"explain", "Explain with AI", "explain", "Explain"}};
        r.tap(r.reader->annotationBar()->actionRect(0));
        CHECK(e.runs.empty());
        CHECK(r.nav.toasts == std::vector<std::string>{"DeepL isn't available right now."});
        CHECK_FALSE(r.reader->pluginResult());
    }
}

TEST_CASE("plugin entries: Working… on the bar tapped again stops the run") {
    FakeEntries e = withActions();
    ReaderRig r(&e);
    r.select();
    r.tap(r.reader->annotationBar()->buttonRect(3));
    r.tap(r.reader->annotationBar()->actionRect(0));
    REQUIRE(r.reader->annotationBar()->working());
    r.tap(r.reader->annotationBar()->buttonRect(3));
    CHECK(e.stops == std::vector<uint64_t>{1});
    CHECK_FALSE(r.reader->pluginResult());
    CHECK_FALSE(r.reader->annotationBar()->working());
}

TEST_CASE("plugin entries: on a PDF (Scribe), the pen writes no ink through the result pop-up") {
    FakeEntries e = withActions();
    ReaderRig r(&e, DocType::Pdf);
    r.select();
    r.tap(r.reader->annotationBar()->buttonRect(3));
    r.tap(r.reader->annotationBar()->actionRect(0));
    e.answer(kLong, {{"note", "Save as note"}});
    r.reader->onTick(r.nav);
    REQUIRE(r.reader->pluginResult());
    REQUIRE(r.reader->pluginResult()->shown());
    CHECK_FALSE(static_cast<const Screen&>(*r.reader).wantsSystemInk());
    const Rect p = r.reader->pluginResult()->rect();
    const Point a{p.x + 200, p.y + 150}, b{p.x + 400, p.y + 200};
    auto pen = [](PenEvent::Type t, Point at) {
        PenEvent ev;
        ev.type = t;
        ev.pos = at;
        return ev;
    };
    r.nav.painted.clear();
    r.reader->onPen(pen(PenEvent::Type::Near, a), r.nav);
    r.reader->onPen(pen(PenEvent::Type::Down, a), r.nav);
    r.reader->onPen(pen(PenEvent::Type::Move, b), r.nav);
    r.reader->onPen(pen(PenEvent::Type::Up, b), r.nav);
    CHECK(r.nav.painted.empty());
    CHECK(r.reader->pendingInkStrokes() == 0);
    r.nav.now += ReaderScreen::kInkCommitMs + 200;
    for (int i = 0; i < 20; ++i) r.reader->onTick(r.nav);
    for (const auto& an : r.lib.annotationsFor("EP000003")) CHECK(an.type != "ink");
    CHECK(r.reader->pluginResult());  // still showing
}

// ---- Home and the launcher ---------------------------------------------------------------------

TEST_CASE("plugin entries: Home shows the puzzle piece only with a main screen, a dot with a badge; the launcher lists them") {
    if (!fonts()) return;
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 40);
    SyncStatus status;
    HomeScreen home{status, HomeActions{}, [] { return int64_t{1'790'000'000'000}; }};
    FakeEntries e = withActions();  // actions only: no icon
    nav.entries = &e;
    home.onShow(nav);
    CHECK_FALSE(home.pluginsIcon());
    home.onGesture(tapAt({HomeScreen::pluginsRect().x + 40, HomeScreen::pluginsRect().y + 40}), nav);
    CHECK(nav.pushed.empty());

    e.launch = {{"riffle", "Feed Riffle", "deck", "Riffle", "12 new"}, {"md", "Highlights to Markdown", "status", "Markdown export", ""}};
    ++e.v;
    nav.damages.clear();
    home.onTick(nav);
    CHECK(home.pluginsIcon());
    CHECK_FALSE(home.pluginsDot());
    REQUIRE(nav.damages.size() == 1);
    CHECK(nav.damages[0].rect == HomeScreen::pluginsRect());
    e.badge = true;
    ++e.v;
    home.onTick(nav);
    CHECK(home.pluginsDot());
    Canvas c(1236, 1547);
    home.draw(c);
    checkGolden(c, "tests/golden/home-plugins.png");
    CHECK(HomeScreen::pluginsRect().x + HomeScreen::pluginsRect().w <= HomeScreen::settingsRect().x);

    home.onGesture(tapAt({HomeScreen::pluginsRect().x + 40, HomeScreen::pluginsRect().y + 40}), nav);
    REQUIRE(nav.pushed.size() == 1);
    auto* launcher = dynamic_cast<PluginLauncherScreen*>(nav.pushed[0].get());
    REQUIRE(launcher);
    launcher->onShow(nav);
    REQUIRE(launcher->rowRects().size() == 2);
    launcher->draw(c);
    checkGolden(c, "tests/golden/plugin-launcher.png");
    const Rect row = launcher->rowRects()[0];
    launcher->onGesture(tapAt({row.x + 20, row.y + 20}), nav);
    CHECK(e.opened == std::vector<std::pair<std::string, std::string>>{{"riffle", "deck"}});
}

TEST_CASE("plugin entries: the pop-up, the More… list and the launcher fit on KT4 and Scribe") {
    if (!fonts()) return;
    FakeEntries e = withActions();
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    for (const Metrics* dev : {&kKt4, &kScribe, &kPw5}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        const Rect area{0, 0, dev->width, dev->height};
        PluginResult p(e, e.selection[0], "selectionAction", {}, area);
        e.answer(std::string(kLong) + "\n\n" + kLong, {{"note", "Save as note"}, {"keep", "Keep for later"}});
        p.tick(nav);
        REQUIRE(p.shown());
        CHECK(p.pages() >= 2);
        const Rect r = p.rect();
        CHECK(r.y >= 0);
        CHECK(r.h <= area.h * 11 / 20);
        for (const Rect b : {p.closeRect(), p.buttonRect(0), p.buttonRect(1)}) {
            REQUIRE_FALSE(b.empty());
            CHECK(r.contains({b.x, b.y}));
            CHECK(b.x + b.w <= dev->width - kMargin);
            CHECK(b.y + b.h <= r.y + r.h);
        }
        CHECK(p.closeRect().x + p.closeRect().w <= p.buttonRect(0).x);
        AnnotationBar bar(AnnotationBar::Mode::Create, {100, 600, 400, 60}, "#ffd400", true);
        CHECK(bar.buttonRect(3).x + bar.buttonRect(3).w <= bar.rect().x + bar.rect().w);
        for (const char* label : {"Highlight", "Underline", "More…", "Working…"})
            CHECK(measureText(label, {theme::kSans, 8.5, 700}, 2000, 1).width + 20 <= bar.buttonRect(3).w);
        std::vector<PluginAction> many(12, e.selection[0]);
        bar.showActions(many);
        const Rect page = ReaderScreen::pageArea();
        CHECK(bar.rect().y >= page.y);
        CHECK(bar.rect().y + bar.rect().h <= page.y + page.h);
    }
}

// ---- End to end: PluginEvents over a real host --------------------------------------------------

namespace {

struct Blank : Screen {
    void draw(Canvas& c) override { fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255); }
    void onGesture(const Gesture&, Nav&) override {}
};

}  // namespace

TEST_CASE("plugin entries: PluginEvents lists a running plugin's entries (waking it), runs actions, opens a run's screen") {
    FakePlugins fp;
    fp.add("riffle", R"JS(
zk.itemAction({ id: "count", title: "Count", run: (item, again) => again ? undefined : { text: `${item.key}!`, actions: [{ id: "x", title: "X" }] } });
zk.itemAction({ id: "open", title: "Open", run: () => { zk.ui.open("deck"); } });
zk.itemAction({ id: "hang", title: "Hang", run: () => new Promise(() => {}) });
zk.screen("deck", { title: "Riffle", main: true, render: () => zk.ui.text("deck") });
zk.setBadge("3 new");
)JS", {}, "Feed Riffle");
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [&] { return now; }};
    PluginHostDeps d = fp.deps();
    d.actionBudgetMs = 5000;
    PluginHost host(d);
    host.setEnabled("riffle", true);
    host.start();
    PluginEvents events(&host);
    events.attach(app);
    app.setPluginEntries(&events);
    app.setPollHook([&] { events.poll(app); });
    for (int i = 0; i < App::kTabs; ++i) app.setRoot(i, std::make_unique<Blank>());
    app.start();
    auto step = [&] {
        app.step(0);
        now += 70;
    };

    REQUIRE(eventually([&] { return events.actions("itemAction").size() == 3; }));  // asking woke it
    const auto actions = events.actions("itemAction");
    CHECK(actions[0].pluginName == "Feed Riffle");
    const auto launch = events.mainScreens();
    REQUIRE(launch.size() == 1);
    CHECK(launch[0].title == "Riffle");
    CHECK(eventually([&] { return events.anyBadge(); }));

    uint64_t run = events.run(actions[0], "itemAction", {{"key", "K1"}});
    std::optional<ActionOutcome> out;
    REQUIRE(eventually([&] {
        step();
        return (out = events.take(run)).has_value();
    }));
    CHECK(out->text == "K1!");
    REQUIRE(out->buttons.size() == 1);
    CHECK(out->buttons[0] == std::pair<std::string, std::string>{"x", "X"});
    run = events.run(actions[0], "itemAction", {{"key", "K1"}}, {{"action", "x"}, {"text", "K1!"}});
    REQUIRE(eventually([&] {
        step();
        return (out = events.take(run)).has_value();
    }));
    CHECK(out->none);

    run = events.run(actions[1], "itemAction", {{"key", "K1"}});
    REQUIRE(eventually([&] {
        step();
        return dynamic_cast<const PluginScreen*>(app.top()) != nullptr;
    }));
    CHECK(events.take(run).has_value());

    run = events.run(actions[2], "itemAction", {{"key", "K1"}});
    events.stop(run);
    for (int i = 0; i < 5; ++i) step();
    CHECK_FALSE(events.take(run).has_value());
    CHECK(host.waitIdle(2000));

    run = events.run(actions[2], "itemAction", {{"key", "K1"}});
    events.rebind(nullptr, app);  // the host goes (an update): the run waiting is answered
    out = events.take(run);
    REQUIRE(out);
    CHECK(out->error);
    CHECK(out->text == "Feed Riffle couldn't do that: it stopped");
    CHECK(events.actions("itemAction").empty());
    host.stop();
    app.closeAll();
}
