// A plugin's screen in the app (src/screens/plugin_screen.h): Loading/tree/error states, gestures
// to events, the event queue and "Working…", typing, Back; and end to end with the host
// (PluginEvents routing ScreenOpen/ScreenClose). Goldens on PW5; KT4 gets fit assertions.

#include <doctest/doctest.h>

#include <deque>
#include <filesystem>

#include "app/app.h"
#include "app/plugin_events.h"
#include "fake_display.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fake_sync.h"
#include "golden.h"
#include "screens/plugin_screen.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
using Kind = PluginHostEvent::Kind;

namespace {

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

struct FakeLink : ScreenLink {
    std::vector<json> sent;
    std::vector<uint64_t> leftScreens;
    std::deque<PluginHostEvent> mail;
    void send(uint64_t, json e) override { sent.push_back(std::move(e)); }
    void left(uint64_t instance) override { leftScreens.push_back(instance); }
    bool take(uint64_t, PluginHostEvent& out) override {
        if (mail.empty()) return false;
        out = std::move(mail.front());
        mail.pop_front();
        return true;
    }
};

PluginHostEvent treeEvent(const json& j, uint64_t requestId = 0) {
    std::string why;
    auto t = parseTree(j, &why);
    INFO(why);
    REQUIRE(t);
    PluginHostEvent ev{Kind::ScreenTree, "riffle", "Feed Riffle", "", {}, requestId, ""};
    ev.instance = 1;
    ev.tree = std::make_shared<const Block>(std::move(*t));
    return ev;
}

json deckScreen(const std::string& title = "Gaussian random fields on manifolds") {
    json j = json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"math.PR · 12 left","style":"label"},
      {"kind":"deck","id":"cards","card":{"kind":"card","title":"","meta":"Adler, Taylor · 2024-01-03",
        "body":"We construct Gaussian random fields on compact Riemannian manifolds.","badge":"In library"}},
      {"kind":"chips","id":"tags","chips":["to-read","spde"],"selected":["to-read"]},
      {"kind":"buttons","buttons":[{"id":"discard","title":"Discard"},{"id":"skip","title":"Skip"},{"id":"keep","title":"Keep","primary":true}]},
      {"kind":"notice","text":"Discarded “Brownian loops”"}
    ]})");
    j["children"][1]["card"]["title"] = title;
    return j;
}

json formScreen() {
    return json::parse(R"({"kind":"column","children":[
      {"kind":"text","text":"Gaussian random fields on manifolds via the Laplace–Beltrami operator","style":"title","lines":2},
      {"kind":"search","id":"find","placeholder":"Collection","focus":true},
      {"kind":"list","id":"collections","rows":[{"id":"c1","title":"Probability / SPDEs"},{"id":"c2","title":"Reading"},
        {"id":"c3","title":"Geometry"},{"id":"c4","title":"Teaching"},{"id":"c5","title":"Archive"}]},
      {"kind":"text","text":"Tags","style":"label"},
      {"kind":"chips","id":"tags","chips":["to-read","spde","random-fields"],"selected":["to-read"]},
      {"kind":"field","id":"tag","placeholder":"Add a tag","value":""},
      {"kind":"field","id":"note","label":"Note","lines":3},
      {"kind":"button","id":"file","title":"File","primary":true}
    ]})");
}

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    FakeLink link;
    PluginScreen screen{link, 1, "Feed Riffle", "Riffle"};
    Rig() {
        nav.visible = &screen;
        screen.onShow(nav);
    }
    void give(PluginHostEvent ev) {
        link.mail.push_back(std::move(ev));
        tick();
    }
    void tick() { screen.onTick(nav); }
    const Laid& laid() {
        REQUIRE(screen.laid());
        return *screen.laid();
    }
    Point centre(Rect r) { return {r.x + r.w / 2, r.y + r.h / 2}; }
    void tap(Point p) { screen.onGesture(tapAt(p), nav); }
    void swipeOn(Point p, Direction d) { screen.onGesture(Gesture{GestureType::Swipe, p, p, d}, nav); }
    void type(const std::string& s) { screen.onKey(KeyEvent{KeyEvent::Code::Text, s}, nav); }
    Rect childRect(int i) { return laid().children.at(i).rect; }
    // The block with this id, turning pages forward until it shows.
    const Laid* seek(const std::string& id) {
        for (int i = 0; i < 8 && !findLaid(laid(), id); ++i) swipeOn({600, 400}, Direction::Up);
        return findLaid(laid(), id);
    }
    void shot(const char* path) {
        Canvas c(metrics().width, metrics().height);
        screen.draw(c);
        checkGolden(c, path);
    }
};

bool inside(Rect outer, Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

}  // namespace

TEST_CASE("plugin screen: Loading, then the tree; a button tap posts {tap}; a deck swipe posts {swipe}") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Rig r;
    CHECK(r.screen.state() == PluginScreen::State::Loading);
    r.shot("tests/golden/plugin-screen-loading.png");
    r.give(treeEvent(deckScreen()));
    CHECK(r.screen.state() == PluginScreen::State::Shown);
    REQUIRE_FALSE(r.nav.damages.empty());

    const Laid& buttons = r.laid().children.at(3);
    r.tap(r.centre(buttons.parts.at(2).rect));
    REQUIRE(r.link.sent.size() == 1);
    CHECK(r.link.sent[0] == json{{"kind", "tap"}, {"id", "keep"}});
    CHECK(r.screen.sending());
    r.give(treeEvent(deckScreen(), 1));
    CHECK_FALSE(r.screen.sending());

    r.swipeOn(r.centre(r.childRect(1)), Direction::Left);
    REQUIRE(r.link.sent.size() == 2);
    CHECK(r.link.sent[1] == json{{"kind", "swipe"}, {"id", "cards"}, {"direction", "left"}});
    // The answer shows another card: only the deck (and what moved) is redrawn, with Quality.
    r.nav.damages.clear();
    r.give(treeEvent(deckScreen("Brownian loops and the central charge"), 2));
    REQUIRE_FALSE(r.nav.damages.empty());
    bool quality = false;
    for (const auto& d : r.nav.damages) {
        CHECK(d.rect.y >= kHeaderH);
        quality = quality || d.waveform == Waveform::Quality;
    }
    CHECK(quality);
    // A list row sends select, a chip sends chips (shown selected at once).
    r.tap(r.centre(r.laid().children.at(2).parts.at(1).rect));
    REQUIRE(r.link.sent.size() == 3);
    CHECK(r.link.sent[2] == json{{"kind", "chips"}, {"id", "tags"}, {"chip", "spde"}, {"selected", {"to-read", "spde"}}});
    CHECK(r.screen.focus().chips.at("tags") == std::vector<std::string>{"to-read", "spde"});
    r.give(treeEvent(deckScreen(), 3));  // the plugin didn't keep it: its selection stands
    CHECK(r.screen.focus().chips.empty());
}

TEST_CASE("plugin screen: [Review Focus] taps during a running onEvent wait in order; \"Working…\" after 1 s; swipes coalesce") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Rig r;
    r.give(treeEvent(deckScreen()));
    const Laid buttons = r.laid().children.at(3);
    r.tap(r.centre(buttons.parts.at(0).rect));  // discard: sent
    r.tap(r.centre(buttons.parts.at(1).rect));  // skip: waits
    r.tap(r.centre(buttons.parts.at(2).rect));  // keep: waits
    CHECK(r.link.sent.size() == 1);
    CHECK(r.screen.waiting() == 2);
    r.nav.now += 999;
    r.tick();
    CHECK_FALSE(r.screen.working());
    r.nav.damages.clear();
    r.nav.now += 1;
    r.tick();
    CHECK(r.screen.working());
    REQUIRE(r.nav.damages.size() == 1);
    CHECK(r.nav.damages[0].rect == PluginScreen::workingRect());
    r.shot("tests/golden/plugin-screen-working.png");

    r.give(treeEvent(deckScreen(), 1));
    CHECK_FALSE(r.screen.working());
    REQUIRE(r.link.sent.size() == 2);
    CHECK(r.link.sent[1]["id"] == "skip");
    r.give(treeEvent(deckScreen(), 2));
    REQUIRE(r.link.sent.size() == 3);
    CHECK(r.link.sent[2]["id"] == "keep");

    // Rapid swipes while keep runs: at most one waiting per direction, in the order first made.
    const Point deck = r.centre(r.childRect(1));
    for (int i = 0; i < 3; ++i) r.swipeOn(deck, Direction::Left);
    for (int i = 0; i < 2; ++i) r.swipeOn(deck, Direction::Right);
    r.swipeOn(deck, Direction::Left);
    CHECK(r.screen.waiting() == 2);
    r.give(treeEvent(deckScreen(), 3));
    r.give(treeEvent(deckScreen(), 4));
    r.give(treeEvent(deckScreen(), 5));
    REQUIRE(r.link.sent.size() == 5);
    CHECK(r.link.sent[3]["direction"] == "left");
    CHECK(r.link.sent[4]["direction"] == "right");
    CHECK(r.screen.waiting() == 0);
}

TEST_CASE("plugin screen: [Review Focus] typing echoes at once, `change` after 700 ms and before any other event; the keyboard never covers the focused field") {
    if (!fonts()) return;
    for (const Metrics* dev : {&kPw5, &kKt4}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        Rig r;
        r.nav.keyboardFake.fixed = keyboardFallback(metrics(), profileFor(metrics().device, 300));  // KT4: the lower half
        r.give(treeEvent(formScreen()));
        CHECK(r.screen.focus().focused == "find");  // focus: true
        CHECK(r.nav.keyboardFake.shown);
        const Rect kb = r.nav.keyboardFake.bounds();
        REQUIRE_FALSE(kb.empty());
        const Laid* find = findLaid(r.laid(), "find");
        REQUIRE(find);
        CHECK(find->rect.y + find->rect.h <= kb.y);
        CHECK(r.laid().rect.y + r.laid().rect.h <= kb.y);
        if (dev == &kPw5) {
            r.type("p");
            r.type("r");
            r.shot("tests/golden/plugin-screen-typing.png");
        } else {
            r.type("p");
            r.type("r");
        }
        CHECK(r.screen.focus().texts.at("find") == "pr");
        REQUIRE_FALSE(r.nav.damages.empty());
        CHECK(r.nav.damages.back().waveform == Waveform::Fast);
        CHECK(r.nav.damages.back().rect == find->rect);
        CHECK(r.link.sent.empty());
        r.nav.now += 699;
        r.tick();
        CHECK(r.link.sent.empty());
        r.nav.now += 1;
        r.tick();
        REQUIRE(r.link.sent.size() == 1);
        CHECK(r.link.sent[0] == json{{"kind", "change"}, {"id", "find"}, {"text", "pr"}});
        r.give(treeEvent(formScreen(), 1));
        CHECK(r.screen.focus().texts.at("find") == "pr");  // no value: kept

        // The note field, far down: focused, it is laid out above the keyboard.
        const Laid* note = r.seek("note");
        REQUIRE(note);
        r.tap(r.centre(note->rect));
        CHECK(r.screen.focus().focused == "note");
        CHECK(r.nav.keyboardFake.shown);
        note = findLaid(r.laid(), "note");
        REQUIRE(note);
        CHECK(note->rect.y + note->rect.h <= r.nav.keyboardFake.bounds().y);
        // Typed, then File at once: the change goes first.
        r.type("Nice");
        r.screen.onKey(KeyEvent{KeyEvent::Code::Return, ""}, r.nav);  // a new line in a 3-line field
        r.type("x");
        CHECK(r.screen.focus().texts.at("note") == "Nice\nx");
        r.screen.onGesture(Gesture{GestureType::Swipe, {600, 400}, {600, 400}, Direction::Down}, r.nav);
        r.screen.onKey(KeyEvent{KeyEvent::Code::Escape, ""}, r.nav);
        CHECK_FALSE(r.nav.keyboardFake.shown);
        const Laid* file = r.seek("file");
        REQUIRE(file);
        r.tap(r.centre(file->rect));
        REQUIRE(r.link.sent.size() == 2);
        CHECK(r.link.sent[1] == json{{"kind", "change"}, {"id", "note"}, {"text", "Nice\nx"}});
        r.give(treeEvent(formScreen(), 2));
        REQUIRE(r.link.sent.size() == 3);
        CHECK(r.link.sent[2] == json{{"kind", "tap"}, {"id", "file"}});
    }
}

TEST_CASE("plugin screen: Enter submits; `value` replaces the text only when it differs from what was last sent") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Rig r;
    json form = formScreen();
    r.give(treeEvent(form));
    const Laid* tag = r.seek("tag");
    REQUIRE(tag);
    r.tap(r.centre(tag->rect));
    r.type("spde");
    r.screen.onKey(KeyEvent{KeyEvent::Code::Return, ""}, r.nav);
    REQUIRE(r.link.sent.size() == 1);
    CHECK(r.link.sent[0] == json{{"kind", "change"}, {"id", "tag"}, {"text", "spde"}});
    r.give(treeEvent(form, 1));  // still value "": differs from "spde" → cleared
    CHECK(r.screen.focus().texts.at("tag") == "");
    REQUIRE(r.link.sent.size() == 2);
    CHECK(r.link.sent[1] == json{{"kind", "submit"}, {"id", "tag"}, {"text", "spde"}});
    r.type("ra");  // typing while value "" comes back again: kept
    r.give(treeEvent(form, 2));
    CHECK(r.screen.focus().texts.at("tag") == "ra");
}

TEST_CASE("plugin screen: the error state, with Back; Back always closes and tells the plugin") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    SUBCASE("error") {
        Rig r;
        r.give(treeEvent(formScreen()));
        CHECK(r.nav.keyboardFake.shown);
        PluginHostEvent e{Kind::ScreenError, "riffle", "Feed Riffle", "Feed Riffle can't show this screen right now", {}, 0, ""};
        r.give(e);
        CHECK(r.screen.state() == PluginScreen::State::Failed);
        CHECK_FALSE(r.nav.keyboardFake.shown);
        r.shot("tests/golden/plugin-screen-error.png");
        r.give(treeEvent(deckScreen(), 1));  // nothing more is drawn
        CHECK(r.screen.state() == PluginScreen::State::Failed);
        r.tap(r.centre(PluginScreen::backButtonRect()));
        CHECK(r.nav.pops == 1);
        r.screen.onClose(r.nav);
        CHECK(r.link.leftScreens == std::vector<uint64_t>{1});
    }
    SUBCASE("while loading") {
        Rig r;
        r.tap(r.centre(headerBackRect()));
        CHECK(r.nav.pops == 1);
    }
    SUBCASE("while an event runs") {
        Rig r;
        r.give(treeEvent(deckScreen()));
        r.tap(r.centre(r.laid().children.at(3).parts.at(0).rect));
        r.tap(r.centre(r.laid().children.at(3).parts.at(1).rect));
        r.tap(r.centre(headerBackRect()));
        CHECK(r.nav.pops == 1);
        r.screen.onClose(r.nav);
        CHECK(r.link.sent.size() == 1);  // the waiting tap is dropped
        CHECK(r.link.leftScreens.size() == 1);
    }
    SUBCASE("an onEvent error shows in the action error pop-up; Close takes it away; Back still works") {
        Rig r;
        r.give(treeEvent(deckScreen()));
        r.give(PluginHostEvent{Kind::ScreenActionError, "riffle", "Feed Riffle", "Feed Riffle couldn't do that: boom", {}, 0, ""});
        CHECK(r.nav.toasts.empty());
        REQUIRE(r.screen.errorPopup());
        CHECK(r.screen.errorPopup()->text() == "Feed Riffle couldn't do that: boom");
        CHECK(r.screen.state() == PluginScreen::State::Shown);
        const size_t sent = r.link.sent.size();
        r.tap(r.centre(r.laid().children.at(3).parts.at(0).rect));  // under the pop-up: not a tap on the screen
        CHECK(r.link.sent.size() == sent);
        r.tap(r.centre(r.screen.errorPopup()->closeRect()));
        CHECK_FALSE(r.screen.errorPopup());
        r.give(PluginHostEvent{Kind::ScreenActionError, "riffle", "Feed Riffle", "Feed Riffle couldn't do that: again", {}, 0, ""});
        r.tap(r.centre(headerBackRect()));
        CHECK(r.nav.pops == 1);
    }
}

TEST_CASE("plugin screen fix: the onEvent error pop-up hides the keyboard, so its Close is never under it") {
    for (const Metrics* dev : {&kPw5, &kKt4}) {
        ScopedMetrics m(*dev);
        INFO(dev->device);
        Rig r;
        r.nav.keyboardFake.fixed = keyboardFallback(metrics(), profileFor(metrics().device, 300));
        r.give(treeEvent(formScreen()));
        REQUIRE(r.nav.keyboardFake.shown);
        r.type("p");
        r.give(PluginHostEvent{Kind::ScreenActionError, "riffle", "Feed Riffle", "Feed Riffle couldn't do that: boom", {}, 0, ""});
        REQUIRE(r.screen.errorPopup());
        CHECK_FALSE(r.nav.keyboardFake.shown);
        CHECK(r.screen.focus().focused.empty());
        const Rect close = r.screen.errorPopup()->closeRect();
        CHECK(close.y + close.h <= metrics().height);
        r.tap(r.centre(close));
        CHECK_FALSE(r.screen.errorPopup());
        bool changeSent = false;  // what was typed still reaches the plugin
        for (int i = 0; i < 4; ++i) r.tick();
        for (const auto& e : r.link.sent) changeSent |= e["kind"] == "change" && e["text"] == "p";
        CHECK(changeSent);
    }
}

// ---- End to end: the app, the host and PluginEvents ----------------------------------------------

namespace {

struct Blank : Screen {
    void draw(Canvas& c) override { fillRect(c.cr(), {0, 0, c.width(), c.height()}, 255); }
    void onGesture(const Gesture&, Nav&) override {}
};

struct AppRig {
    FakePlugins fp;
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeDisplay display;
    FakeSync sync;
    FakeKeyboard keyboard;
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [this] { return now; }};
    std::unique_ptr<PluginHost> host;
    std::unique_ptr<PluginEvents> events;

    explicit AppRig(const std::string& js) {
        fp.add("riffle", js, {}, "Feed Riffle");
        PluginHostDeps d = fp.deps();
        d.renderBudgetMs = 500;
        d.actionBudgetMs = 1000;
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("riffle", true);
        host->start();
        events = std::make_unique<PluginEvents>(host.get());
        app.setPollHook([this] { events->poll(app); });
        for (int i = 0; i < App::kTabs; ++i) app.setRoot(i, std::make_unique<Blank>());
        app.start();
    }
    ~AppRig() {
        host->stop();
        app.closeAll();  // the screens before the PluginEvents they talk to
    }
    void step() {
        app.step(0);
        now += 70;
    }
    bool until(const std::function<bool()>& cond) {
        return eventually([&] {
            step();
            return cond();
        });
    }
    const PluginScreen* top() const { return dynamic_cast<const PluginScreen*>(app.top()); }
    bool shown(const std::string& text) {
        const PluginScreen* s = top();
        if (!s || !s->laid()) return false;
        const Block& root = *s->laid()->block;
        return root.kind == Block::Kind::Column && plainText(root.children.at(0)) == text;
    }
    void tapButton(const std::string& id) {
        const Laid* b = findLaid(*top()->laid(), id);
        REQUIRE(b);
        display.tap({b->rect.x + b->rect.w / 2, b->rect.y + b->rect.h / 2}, now);
        step();
        step();
    }
};

const char* kRiffle = R"JS(
let n = 0, last = "-";
zk.screen("deck", {
  title: "Riffle",
  main: true,
  render: () => zk.ui.column([zk.ui.text(`n=${n} last=${last}`), zk.ui.button({ id: "more", title: "More" }), zk.ui.button({ id: "file", title: "File" })]),
  onEvent(e) {
    if (e.kind === "tap" && e.id === "more") n++;
    if (e.kind === "tap" && e.id === "file") zk.ui.open("file", { n });
    if (e.kind === "result") last = String(e.value);
  },
});
zk.screen("file", {
  title: "File",
  render: (p) => zk.ui.column([zk.ui.text(`filing ${p.n}`), zk.ui.button({ id: "done", title: "Done" })]),
  onEvent(e, p) { if (e.kind === "tap" && e.id === "done") zk.ui.close(`filed ${p.n}`); },
});
)JS";

}  // namespace

TEST_CASE("plugin screen, end to end: open, tap, zk.ui.open pushes a screen, zk.ui.close pops it with its result, Back") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(kRiffle);
    const uint64_t s = r.events->openScreen(r.app, "riffle", "deck");
    CHECK(s != 0);
    REQUIRE(r.top());
    CHECK(r.top()->state() == PluginScreen::State::Loading);
    REQUIRE(r.until([&] { return r.shown("n=0 last=-"); }));
    r.tapButton("more");
    REQUIRE(r.until([&] { return r.shown("n=1 last=-"); }));
    r.tapButton("file");
    REQUIRE(r.until([&] { return r.shown("filing 1"); }));
    r.tapButton("done");
    REQUIRE(r.until([&] { return r.shown("n=1 last=filed 1"); }));
    r.tapButton("file");
    REQUIRE(r.until([&] { return r.shown("filing 1"); }));
    r.display.tap({headerBackRect().x + 20, headerBackRect().y + 20}, r.now);  // Back
    REQUIRE(r.until([&] { return r.shown("n=1 last=undefined"); }));
    r.display.tap({headerBackRect().x + 20, headerBackRect().y + 20}, r.now);
    REQUIRE(r.until([&] { return r.top() == nullptr; }));
}

TEST_CASE("plugin screen, end to end: [Review Focus] the plugin turned off while its screen shows → the error state, Back works") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(kRiffle);
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("n=0 last=-"); }));
    r.host->setEnabled("riffle", false);
    REQUIRE(r.until([&] { return r.top() && r.top()->state() == PluginScreen::State::Failed; }));
    CHECK(r.top()->errorText() == "Feed Riffle can't show this screen right now");
    r.display.tap({headerBackRect().x + 20, headerBackRect().y + 20}, r.now);
    REQUIRE(r.until([&] { return r.top() == nullptr; }));

    // The host goes (an update's install): open screens show the error too.
    r.host->setEnabled("riffle", true);
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("n=0 last=-"); }));
    r.events->rebind(nullptr, r.app);
    REQUIRE(r.until([&] { return r.top() && r.top()->state() == PluginScreen::State::Failed; }));
}

TEST_CASE("plugin screen, end to end: plugins not running (safe mode, an install) or no host → the error state at once, Back works") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(kRiffle);
    r.events->setRunning(false);
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.top() && r.top()->state() == PluginScreen::State::Failed; }));
    CHECK(r.top()->errorText() == "Feed Riffle can't show this screen right now");
    r.display.tap({headerBackRect().x + 20, headerBackRect().y + 20}, r.now);
    REQUIRE(r.until([&] { return r.top() == nullptr; }));

    r.events->setRunning(true);
    r.events->rebind(nullptr, r.app);
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.top() && r.top()->state() == PluginScreen::State::Failed; }));
    CHECK(r.top()->errorText() == "riffle can't show this screen right now");
    r.display.tap({headerBackRect().x + 20, headerBackRect().y + 20}, r.now);
    REQUIRE(r.until([&] { return r.top() == nullptr; }));
}

TEST_CASE("plugin screen, end to end: leaving the tab closes its plugin screens (the plugin hears back)") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(R"JS(
let backs = 0;
zk.screen("deck", { title: "Riffle", main: true,
  render: () => zk.ui.column([zk.ui.text(`backs=${backs}`), zk.ui.button({ id: "more", title: "More" })]),
  onEvent(e) { if (e.kind === "back") backs++; } });
)JS");
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("backs=0"); }));
    r.app.switchTab(1);
    r.step();
    r.app.switchTab(0);
    r.step();
    CHECK(r.top() == nullptr);  // the tab's root again
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("backs=1"); }));
}
