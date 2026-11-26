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
    std::vector<uint64_t> leftScreens, rerendered;
    std::deque<PluginHostEvent> mail;
    void send(uint64_t, json e) override { sent.push_back(std::move(e)); }
    void left(uint64_t instance) override { leftScreens.push_back(instance); }
    void rerender(uint64_t instance) override { rerendered.push_back(instance); }
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

    explicit AppRig(const std::string& js, const std::vector<std::string>& perms = {}) {
        fp.add("riffle", js, perms, "Feed Riffle");
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

TEST_CASE("plugin screen: a tap on the deck's card posts {tap, id: the deck}; a notice with an id posts {tap}; one without stays quiet") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    Rig r;
    json j = deckScreen();
    j["children"][4]["id"] = "kept";
    j["children"].push_back(json{{"kind", "notice"}, {"text", "A notice without an id"}});
    r.give(treeEvent(j));
    r.tap(r.centre(r.childRect(1)));
    REQUIRE(r.link.sent.size() == 1);
    CHECK(r.link.sent[0] == json{{"kind", "tap"}, {"id", "cards"}});
    r.give(treeEvent(j, 1));
    r.tap(r.centre(r.childRect(4)));
    REQUIRE(r.link.sent.size() == 2);
    CHECK(r.link.sent[1] == json{{"kind", "tap"}, {"id", "kept"}});
    r.give(treeEvent(j, 2));
    r.tap(r.centre(r.childRect(5)));
    CHECK(r.link.sent.size() == 2);
}

TEST_CASE("plugin screen, end to end: zk.ui.quote draws a quote; a tap on one with an id brings {tap, id}") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(R"JS(
let said = "-";
zk.screen("deck", {
  title: "Riffle",
  main: true,
  render: () => zk.ui.column([
    zk.ui.text(said),
    zk.ui.quote({ id: "passage", text: "It is a truth universally acknowledged.", source: "Pride and Prejudice · p. 1" }),
    zk.ui.quote({ text: "Call me Ishmael." }),
  ]),
  onEvent(e) { if (e.kind === "tap") said = "tapped " + e.id; },
});
)JS");
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("-"); }));
    const Block& root = *r.top()->laid()->block;
    REQUIRE(root.children.size() == 3);
    CHECK(root.children[1].kind == Block::Kind::Quote);
    CHECK(root.children[1].meta == "Pride and Prejudice · p. 1");
    CHECK(root.children[2].kind == Block::Kind::Quote);
    CHECK(root.children[2].id.empty());
    r.tapButton("passage");
    REQUIRE(r.until([&] { return r.shown("tapped passage"); }));
}

TEST_CASE("plugin screen, end to end: a tap on a text or a box with an id brings {tap, id}, as a quote's does") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(R"JS(
let said = "-";
zk.screen("deck", {
  title: "Riffle",
  main: true,
  render: () => zk.ui.column([
    zk.ui.text(said),
    zk.ui.text("All 9 ›", { id: "all-pinned", weight: "bold" }),
    zk.ui.box(zk.ui.text("Pinned"), { id: "pinned", border: "thin", pad: "small" }),
  ]),
  onEvent(e) { if (e.kind === "tap") said = "tapped " + e.id; },
});
)JS");
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("-"); }));
    r.tapButton("all-pinned");
    REQUIRE(r.until([&] { return r.shown("tapped all-pinned"); }));
    r.tapButton("pinned");
    REQUIRE(r.until([&] { return r.shown("tapped pinned"); }));
}

TEST_CASE("plugin screen, end to end: zk.reader.open opens over the screen; the promise resolves; Back brings a `reader` event") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(R"JS(
let said = "-";
zk.screen("deck", {
  title: "Riffle",
  main: true,
  render: () => zk.ui.column([zk.ui.text(said), zk.ui.button({ id: "read", title: "Read" })]),
  async onEvent(e) {
    if (e.kind === "tap") { await zk.reader.open({ url: "https://arxiv.org/pdf/2401.01234v2", title: "A paper" }); said = "opened"; }
    if (e.kind === "reader") said = "back from the reader";
  },
});
)JS", {"net:arxiv.org"});
    struct Shown final : Screen {  // stands in for the reader
        std::function<void(nlohmann::json)> closed;
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
        void onClose(Nav&) override { closed(nullptr); }
    };
    nlohmann::json asked;
    r.events->setPreviewOpener([&](App& a, const PluginHostEvent& ev, std::function<void(const std::string&)> answer,
                                   std::function<void(nlohmann::json)> closed) {
        asked = ev.value;
        auto s = std::make_unique<Shown>();
        s->closed = std::move(closed);
        a.push(std::move(s));
        answer("");
    });
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("-"); }));
    r.tapButton("read");
    REQUIRE(r.until([&] { return dynamic_cast<const Shown*>(r.app.top()) != nullptr; }));
    CHECK(asked["url"] == "https://arxiv.org/pdf/2401.01234v2");
    r.app.pop();  // Back, out of the reader
    REQUIRE(r.until([&] { return r.shown("back from the reader"); }));
}

TEST_CASE("plugin screen, end to end: zk.reader.open with save passes action and annotations on close") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(R"JS(
let said = "-";
zk.screen("deck", {
  title: "Gazette",
  main: true,
  render: () => zk.ui.column([zk.ui.text(said), zk.ui.button({ id: "read", title: "Read" })]),
  async onEvent(e) {
    if (e.kind === "tap") { await zk.reader.open({ html: "<p>Hi</p>", title: "A note", save: "prompt" }); said = "opened"; }
    if (e.kind === "reader") said = `${e.action}:${(e.annotations || []).length}:${e.annotations?.[0]?.text ?? ""}`;
  },
});
)JS");
    struct Shown final : Screen {
        std::function<void(nlohmann::json)> closed;
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
        void onClose(Nav&) override {
            closed({{"action", "save"},
                    {"annotations", nlohmann::json::array({{{"type", "highlight"}, {"text", "hello world"}, {"color", "#ffd400"}}})}});
        }
    };
    nlohmann::json asked;
    r.events->setPreviewOpener([&](App& a, const PluginHostEvent& ev, std::function<void(const std::string&)> answer,
                                   std::function<void(nlohmann::json)> closed) {
        asked = ev.value;
        auto s = std::make_unique<Shown>();
        s->closed = std::move(closed);
        a.push(std::move(s));
        answer("");
    });
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("-"); }));
    r.tapButton("read");
    REQUIRE(r.until([&] { return dynamic_cast<const Shown*>(r.app.top()) != nullptr; }));
    CHECK(asked["save"] == "prompt");
    r.app.pop();
    REQUIRE(r.until([&] { return r.shown("save:1:hello world"); }));
}

// ---- Items (the `items` block): taps open the item the app's way, long-press shows the sheet -------

#include "screens/item_source.h"
#include "fixture_library.h"

namespace {

struct ItemsRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    FakeLink link;
    std::unique_ptr<LibraryItemSource> source;
    std::vector<ItemRow> opened;
    std::string outcome = "";  // what the open reports: "" the reader showed
    std::unique_ptr<PluginScreen> screen;
    ItemsRig() {
        fillFixtureLibrary(lib, 8);
        source = std::make_unique<LibraryItemSource>(lib, nullptr, [] { return int64_t{0}; });
        screen = std::make_unique<PluginScreen>(link, 1, "Feed Riffle", "Riffle", source.get(),
                                                ItemTap{[this](const ItemRow& r, std::function<void(const std::string&)> done) {
                                                    opened.push_back(r);
                                                    if (done) done(outcome);
                                                }});
        nav.visible = screen.get();
        screen->onShow(nav);
        link.mail.push_back(treeEvent(json::parse(R"({"kind":"column","children":[
          {"kind":"items","id":"recent","style":"row","items":["IT000000","GONE","IT000003"]}]})"), 0));
        screen->onTick(nav);
    }
    Point first() {
        const Laid* l = findLaid(*screen->laid(), "recent");
        REQUIRE(l);
        REQUIRE(l->parts.size() == 2);
        return {l->parts[0].rect.x + 20, l->parts[0].rect.y + l->parts[0].rect.h / 2};
    }
};

}  // namespace

TEST_CASE("plugin screen items: a tap on a row opens that item through ItemTap and tells the plugin nothing") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    ItemsRig r;
    r.screen->onGesture(tapAt(r.first()), r.nav);
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened[0].key == "IT000000");
    CHECK(r.link.sent.empty());
}

TEST_CASE("plugin screen items: once an item was opened, showing again tells the plugin the reader closed") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    ItemsRig r;
    r.screen->onShow(r.nav);
    r.screen->onTick(r.nav);
    CHECK(r.link.sent.empty());  // nothing opened yet
    r.screen->onGesture(tapAt(r.first()), r.nav);
    r.screen->onShow(r.nav);  // the reader closed
    r.screen->onTick(r.nav);
    REQUIRE(r.link.sent.size() == 1);
    CHECK(r.link.sent[0] == json{{"kind", "reader"}, {"closed", true}});
}

TEST_CASE("plugin screen items: an open that failed or was cancelled sends no reader event when the screen shows again") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    ItemsRig r;
    r.outcome = "Cancelled";
    r.screen->onGesture(tapAt(r.first()), r.nav);
    r.screen->onShow(r.nav);
    r.screen->onTick(r.nav);
    CHECK(r.link.sent.empty());
}

TEST_CASE("plugin screen items: a long-press shows the item sheet; Close hides it; Open opens through ItemTap") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    ItemsRig r;
    r.screen->onGesture(Gesture{GestureType::LongPress, r.first(), r.first()}, r.nav);
    REQUIRE(r.screen->sheet());
    CHECK(r.screen->sheet()->row().key == "IT000000");
    CHECK(r.link.sent.empty());
    const Rect area{0, 0, metrics().width, metrics().height};
    r.screen->onGesture(tapAt({metrics().width / 2, 20}), r.nav);  // above the sheet
    CHECK_FALSE(r.screen->sheet());
    r.screen->onGesture(Gesture{GestureType::LongPress, r.first(), r.first()}, r.nav);
    REQUIRE(r.screen->sheet());
    const Rect open = r.screen->sheet()->openRect(area);
    r.screen->onGesture(tapAt({open.x + open.w / 2, open.y + open.h / 2}), r.nav);
    CHECK_FALSE(r.screen->sheet());
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened[0].key == "IT000000");
}

TEST_CASE("plugin screen items: Mark as read in the sheet reads the rows again, with no call to the plugin") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    ItemsRig r;
    r.screen->onGesture(Gesture{GestureType::LongPress, r.first(), r.first()}, r.nav);
    REQUIRE(r.screen->sheet());
    REQUIRE_FALSE(r.screen->laid()->children.at(0).items.at(0).read);
    const Rect area{0, 0, metrics().width, metrics().height};
    const Rect read = r.screen->sheet()->readRect(area);
    r.screen->onGesture(tapAt({read.x + read.w / 2, read.y + read.h / 2}), r.nav);
    CHECK(r.screen->laid()->children.at(0).items.at(0).read);
    CHECK(r.link.sent.empty());
    CHECK(r.link.rerendered == std::vector<uint64_t>{1});  // the plugin renders again (the item may leave its list)
}

TEST_CASE("plugin screen, end to end: zk.library.open opens through the item opener; unknown → \"Item not found\"; Back brings a `reader` event") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(R"JS(
let said = "-";
zk.screen("deck", {
  title: "Riffle",
  main: true,
  render: () => zk.ui.column([zk.ui.text(said), zk.ui.button({ id: "read", title: "Read" }), zk.ui.button({ id: "lost", title: "Lost" })]),
  async onEvent(e) {
    if (e.kind === "tap") {
      try { await zk.library.open(e.id === "read" ? { key: "BOOK0001", library: 0 } : "GONE", { attachment: "FILE0001", annotation: "NOTE0001" }); said = "opened"; }
      catch (err) { said = "rejected: " + err.message; }
    }
    if (e.kind === "reader") said = "back from the reader";
  },
});
)JS", {"library:read"});
    struct Shown final : Screen {
        void draw(Canvas&) override {}
        void onGesture(const Gesture&, Nav&) override {}
    };
    ItemRef asked;
    std::optional<std::string> file, note;
    r.events->setItemOpener([&](App& a, const ItemRef& ref, std::optional<std::string> attachment, std::optional<std::string> annotation,
                                std::function<void(const std::string&)> answer) {
        if (ref.key == "GONE") return answer("Item not found");
        asked = ref;
        file = attachment;
        note = annotation;
        a.push(std::make_unique<Shown>());
        answer("");
    });
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("-"); }));
    r.tapButton("lost");
    REQUIRE(r.until([&] { return r.shown("rejected: Item not found"); }));
    r.tapButton("read");
    REQUIRE(r.until([&] { return dynamic_cast<const Shown*>(r.app.top()) != nullptr; }));
    CHECK(asked.key == "BOOK0001");
    CHECK(asked.library == 0);
    CHECK(file == "FILE0001");
    CHECK(note == "NOTE0001");
    r.app.pop();  // out of the reader
    REQUIRE(r.until([&] { return r.shown("back from the reader"); }));
}

// PluginBody on its own, as Home's body uses it: no header, laid out in the owner's Rect.
TEST_CASE("plugin body: laid out in the Rect it is given; taps queue as on a screen; Working… at the Rect's top") {
    ScopedMetrics m(kPw5);
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    FakeLink link;
    const Rect area{40, 500, metrics().width - 80, 700};
    PluginBody body(link, 1, "Feed Riffle", "Desk", [&](Nav&) { return area; }, TreeUse::HomeBody);
    body.onShow(nav);
    CHECK(body.state() == PluginBody::State::Loading);
    CHECK_FALSE(body.laid());
    link.mail.push_back(treeEvent(deckScreen()));
    body.onTick(nav);
    REQUIRE(body.state() == PluginBody::State::Shown);
    REQUIRE(body.laid());
    for (const Laid& c : body.laid()->children) CHECK(inside(area, c.rect));
    const Laid buttons = body.laid()->children.at(3);
    const Rect keep = buttons.parts.at(2).rect;
    body.onGesture(tapAt({keep.x + keep.w / 2, keep.y + keep.h / 2}), nav);
    REQUIRE(link.sent.size() == 1);
    CHECK(link.sent[0] == json{{"kind", "tap"}, {"id", "keep"}});
    nav.damages.clear();
    nav.now += PluginBody::kWorkingMs;
    body.onTick(nav);
    CHECK(body.working());
    REQUIRE(nav.damages.size() == 1);
    CHECK(nav.damages[0].rect == PluginBody::workingRect(area));
    CHECK(PluginBody::workingRect(area).y == area.y + 4);
    CHECK(PluginScreen::workingRect() == PluginBody::workingRect(blocksArea()));  // where a screen's always was
    body.onClose(nav);
    CHECK(link.leftScreens == std::vector<uint64_t>{1});
}

TEST_CASE("plugin body: for Home (TreeUse::HomeBody) a tree with a search or field fails it, no keyboard; a screen's takes it") {
    ScopedMetrics m(kPw5);
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    FakeLink link;
    PluginBody home(link, 1, "Feed Riffle", "Desk", [](Nav&) { return blocksArea(); }, TreeUse::HomeBody);
    link.mail.push_back(treeEvent(formScreen()));  // a focused search
    home.onTick(nav);
    CHECK(home.state() == PluginBody::State::Failed);
    CHECK(home.errorText() == "Feed Riffle can't show this screen right now");
    CHECK_FALSE(home.wantsKeyboard());
    CHECK_FALSE(nav.keyboardFake.shown);
    link.mail.push_back(treeEvent(deckScreen()));  // nothing more is shown once failed
    home.onTick(nav);
    CHECK(home.state() == PluginBody::State::Failed);

    PluginBody screen(link, 2, "Feed Riffle", "Form", [](Nav&) { return blocksArea(); });
    link.mail.push_back(treeEvent(formScreen()));
    screen.onTick(nav);
    CHECK(screen.state() == PluginBody::State::Shown);
    CHECK(screen.wantsKeyboard());
}

TEST_CASE("plugin body: the plugin closing its screen stops the body and tells its owner") {
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    FakeLink link;
    PluginBody body(link, 1, "Feed Riffle", "Desk", [](Nav&) { return blocksArea(); }, TreeUse::HomeBody);
    int closes = 0;
    body.onPluginClose([&](Nav&) { ++closes; });
    link.mail.push_back(treeEvent(deckScreen()));
    PluginHostEvent close{Kind::ScreenClose, "riffle", "Feed Riffle", "", {}, 0, ""};
    close.instance = 1;
    link.mail.push_back(close);
    link.mail.push_back(treeEvent(deckScreen()));
    body.onTick(nav);
    CHECK(closes == 1);
    CHECK(body.closedByPlugin());
    CHECK(link.mail.size() == 1);  // nothing more taken
    CHECK(nav.pops == 0);          // the owner decides
    const Rect keep = body.laid()->children.at(3).parts.at(2).rect;
    body.onGesture(tapAt({keep.x + keep.w / 2, keep.y + keep.h / 2}), nav);  // Home keeps showing it meanwhile
    body.onShow(nav);
    body.onTick(nav);
    CHECK(link.sent.empty());  // nothing goes to a screen the plugin closed
    body.onClose(nav);
    CHECK(link.leftScreens.empty());  // the plugin closed it: no `left`
}

TEST_CASE("plugin screen items: a tap on an item piece opens it through ItemTap; a long-press shows its sheet") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    ItemsRig r;
    r.link.mail.push_back(treeEvent(json::parse(R"({"kind":"column","children":[
      {"kind":"item","item":"IT000003","child":{"kind":"row","children":[{"kind":"cover","item":"IT000003","width":"20%"},{"kind":"text","text":"Third"}]}},
      {"kind":"item","item":"GONE","child":{"kind":"text","text":"Gone"}}]})"), 0));
    r.screen->onTick(r.nav);
    const Laid& laid = *r.screen->laid();
    const Point at = laid.children.at(0).rect.center();
    r.screen->onGesture(tapAt(at), r.nav);
    REQUIRE(r.opened.size() == 1);
    CHECK(r.opened[0].key == "IT000003");
    CHECK(r.link.sent.empty());
    r.screen->onGesture(Gesture{GestureType::LongPress, at, at}, r.nav);
    REQUIRE(r.screen->sheet());
    CHECK(r.screen->sheet()->row().key == "IT000003");
    r.screen->onGesture(tapAt({metrics().width / 2, 20}), r.nav);  // above the sheet: closed
    REQUIRE_FALSE(r.screen->sheet());
    // An unresolved ref: inert.
    const Point gone = r.screen->laid()->children.at(1).rect.center();
    r.screen->onGesture(tapAt(gone), r.nav);
    r.screen->onGesture(Gesture{GestureType::LongPress, gone, gone}, r.nav);
    CHECK(r.opened.size() == 1);
    CHECK_FALSE(r.screen->sheet());
    CHECK(r.link.sent.empty());
}

TEST_CASE("plugin screen, end to end: images come from the plugin's package, on a screen opened by the app and by zk.ui.open") {
    if (!fonts()) return;
    ScopedMetrics m(kPw5);
    AppRig r(R"JS(
zk.screen("deck", {
  title: "Riffle",
  main: true,
  render: () => zk.ui.column([zk.ui.text("deck"), zk.ui.image({ src: "pic.png", id: "pic" }), zk.ui.button({ id: "more", title: "More" })]),
  onEvent(e) { if (e.kind === "tap") zk.ui.open("more", {}); },
});
zk.screen("more", { title: "More", render: () => zk.ui.column([zk.ui.text("more"), zk.ui.image({ src: "pic.png", id: "pic", width: "50%" })]) });
)JS");
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 40, 20);
    REQUIRE(cairo_surface_write_to_png(s, r.fp.path("riffle/pic.png").c_str()) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(s);
    r.events->openScreen(r.app, "riffle", "deck");
    REQUIRE(r.until([&] { return r.shown("deck"); }));
    const Rect deck = findLaid(*r.top()->laid(), "pic")->rect;
    CHECK(deck.w == blocksArea().w);
    CHECK(deck.h == deck.w / 2);
    r.tapButton("more");
    REQUIRE(r.until([&] { return r.shown("more"); }));
    const Rect more = findLaid(*r.top()->laid(), "pic")->rect;
    CHECK(more.w == blocksArea().w / 2);
    CHECK(more.h == more.w / 2);
}
