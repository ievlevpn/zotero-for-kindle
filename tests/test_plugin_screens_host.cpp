// Screens on the plugin thread (PluginHost::openScreen/screenEvent/closeScreen; zk.screen, zk.ui):
// the open event, render, events, navigation between a plugin's screens, and every way a screen
// can fail.

#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <functional>
#include <set>
#include <thread>

#include "fake_plugins.h"
#include "plugins/host.h"
#include "plugins/plugin_ctx.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
using Kind = PluginHostEvent::Kind;

namespace {

struct Host {
    FakePlugins fp;
    std::unique_ptr<PluginHost> host;
    std::deque<PluginHostEvent> got;

    explicit Host(const std::string& js, const std::vector<std::string>& perms = {}, int renderMs = 300, int actionMs = 600,
                  const std::function<void(PluginHostDeps&)>& tweak = {}) {
        fp.add("p", js, perms);
        PluginHostDeps d = fp.deps();
        d.renderBudgetMs = renderMs;
        d.actionBudgetMs = actionMs;
        if (tweak) tweak(d);
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("p", true);
        host->start();
    }
    ~Host() { host->stop(); }

    // The next event of `kind` for `instance` (others stay for later), or nullopt after `ms`.
    std::optional<PluginHostEvent> next(Kind kind, uint64_t instance, int ms = 3000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            PluginHostEvent ev;
            while (host->poll(ev)) got.push_back(std::move(ev));
            for (auto it = got.begin(); it != got.end(); ++it)
                if (it->kind == kind && (instance == 0 || it->instance == instance)) {
                    PluginHostEvent out = std::move(*it);
                    got.erase(it);
                    return out;
                }
            if (std::chrono::steady_clock::now() >= end) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // The first text block's text in a tree (a column's first child, or the root).
    static std::string firstText(const PluginHostEvent& ev) {
        REQUIRE(ev.tree);
        const Block& b = ev.tree->kind == Block::Kind::Column ? ev.tree->children.at(0) : *ev.tree;
        return plainText(b);
    }
    int failures() const { return statusOf(host->status(), "p")->failures; }
    std::string log() const { return fp.log("p"); }
};

}  // namespace

TEST_CASE("screens host: open → the open event, then render; an event → onEvent → a new tree answering it") {
    Host h(R"JS(
let n = 0;
zk.screen("main", {
  title: "Counter",
  main: true,
  render: () => zk.ui.column([zk.ui.text(`n=${n}`), zk.ui.button({ id: "more", title: "More" })]),
  onEvent(e) { if (e.kind === "open") n = 10; if (e.kind === "tap" && e.id === "more") n++; },
});
)JS");
    const uint64_t s = h.host->openScreen("p", "main");
    auto t = h.next(Kind::ScreenTree, s);
    REQUIRE(t);
    CHECK(Host::firstText(*t) == "n=10");
    CHECK(t->requestId == 0);
    CHECK(t->pluginName == "P");
    const auto all = h.host->status();
    const auto st = statusOf(all, "p");
    REQUIRE(st->entries.size() == 1);
    CHECK(st->entries[0].title == "Counter");
    CHECK(st->entries[0].main);

    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "more"}});
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "more"}});
    t = h.next(Kind::ScreenTree, s);
    REQUIRE(t);
    if (t->requestId == 1) t = h.next(Kind::ScreenTree, s);  // the events are handled one by one
    REQUIRE(t);
    CHECK(t->requestId == 2);
    CHECK(Host::firstText(*t) == "n=12");
}

TEST_CASE("screens host: render may be async and gets the params; a screen without onEvent renders at once") {
    Host h(R"JS(
zk.screen("s", { title: "S", render: async (p) => { await null; return zk.ui.text(p ? `word ${p.word}` : "none"); } });
)JS");
    auto t = h.next(Kind::ScreenTree, h.host->openScreen("p", "s", {{"word", "hi"}}));
    REQUIRE(t);
    CHECK(Host::firstText(*t) == "word hi");
    t = h.next(Kind::ScreenTree, h.host->openScreen("p", "s"));
    REQUIRE(t);
    CHECK(Host::firstText(*t) == "none");
}

TEST_CASE("screens host: render throwing, over its budget, or an invalid tree → ScreenError, one failure counted") {
    SUBCASE("throws") {
        Host h(R"JS(zk.screen("s", { title: "S", render() { throw new Error("nope"); } });)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        auto e = h.next(Kind::ScreenError, s);
        REQUIRE(e);
        CHECK(e->text == "P can't show this screen right now");
        CHECK(eventually([&] { return h.failures() == 1; }));
        CHECK(h.log().find("nope") != std::string::npos);
    }
    SUBCASE("over budget") {
        Host h(R"JS(zk.screen("s", { title: "S", render() { for (;;) {} } });)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        REQUIRE(h.next(Kind::ScreenError, s));
        CHECK(eventually([&] { return h.failures() == 1; }));
        h.host->waitIdle(2000);
        CHECK(h.failures() == 1);
        CHECK_FALSE(h.next(Kind::ScreenError, s, 200));  // once
    }
    SUBCASE("[Review Focus] an invalid tree after valid ones") {
        Host h(R"JS(
let bad = false, n = 0;
zk.screen("s", {
  title: "S",
  render: () => bad ? zk.ui.column([zk.ui.text("x"), { kind: "toggle", id: "t" }]) : zk.ui.text(`ok ${n}`),
  onEvent(e) { if (e.kind === "tap") { n++; if (e.id === "break") bad = true; } },
});
)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        REQUIRE(h.next(Kind::ScreenTree, s));
        h.host->screenEvent(s, {{"kind", "tap"}, {"id", "a"}});
        auto t = h.next(Kind::ScreenTree, s);
        REQUIRE(t);
        CHECK(Host::firstText(*t) == "ok 1");
        h.host->screenEvent(s, {{"kind", "tap"}, {"id", "break"}});
        auto e = h.next(Kind::ScreenError, s);
        REQUIRE(e);
        CHECK(e->text == "P can't show this screen right now");
        CHECK(h.failures() == 1);
        CHECK(h.log().find("unknown block kind \"toggle\"") != std::string::npos);
        h.host->screenEvent(s, {{"kind", "tap"}, {"id", "a"}});  // nothing more runs for it
        CHECK_FALSE(h.next(Kind::ScreenTree, s, 200));
        CHECK(h.failures() == 1);
    }
    SUBCASE("too big") {
        Host h(R"JS(zk.screen("s", { title: "S", render: () => zk.ui.text("x".repeat(300 * 1024)) });)JS");
        REQUIRE(h.next(Kind::ScreenError, h.host->openScreen("p", "s")));
        CHECK(eventually([&] { return h.failures() == 1; }));
    }
    SUBCASE("no such screen, or the plugin is off") {
        Host h(R"JS(zk.screen("s", { title: "S", render: () => zk.ui.text("x") });)JS");
        CHECK(h.next(Kind::ScreenError, h.host->openScreen("p", "nope")));
        CHECK(h.next(Kind::ScreenError, h.host->openScreen("other", "s")));
        CHECK(h.failures() == 0);
    }
}

TEST_CASE("screens host: an onEvent error is an action error, and the screen renders again") {
    Host h(R"JS(
let n = 0;
zk.screen("s", { title: "S", render: () => zk.ui.text(`n=${n}`), onEvent(e) { if (e.kind === "tap") { n++; throw new Error("boom"); } } });
)JS");
    const uint64_t s = h.host->openScreen("p", "s");
    REQUIRE(h.next(Kind::ScreenTree, s));
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "x"}});
    auto e = h.next(Kind::ScreenActionError, s);
    REQUIRE(e);
    CHECK(e->text == "P couldn't do that: boom");
    auto t = h.next(Kind::ScreenTree, s);
    REQUIRE(t);
    CHECK(Host::firstText(*t) == "n=1");
    CHECK(t->requestId == 1);
}

namespace {
JSValue jsUserCaused(JSContext* c, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(c, plugins::currentCall(plugins::ctxOf(c)).userCaused);
}
}  // namespace

TEST_CASE("screens host: the back event is not user-caused (the screen is gone: nothing is asked)") {
    Host h(R"JS(
const seen = [];
zk.screen("a", { title: "A", render: () => zk.ui.text(seen.join(",") || "-"), onEvent(e) { if (e.kind === "result") seen.push(`result ${e.from}`); if (e.kind === "tap") zk.ui.open("b"); } });
zk.screen("b", { title: "B", render: () => zk.ui.text("b"), onEvent(e) { seen.push(`${e.kind} ${zk.userCaused()}`); } });
)JS", {}, 300, 600, [](PluginHostDeps& d) {
        d.extraBindings.push_back([](plugins::PluginCtx& p) {
            plugins::defineFunction<jsUserCaused>(p.cx.raw(), p.zk.raw(), "userCaused", 0);
        });
    });
    const uint64_t a = h.host->openScreen("p", "a");
    REQUIRE(h.next(Kind::ScreenTree, a));
    h.host->screenEvent(a, {{"kind", "tap"}, {"id", "x"}});
    auto o = h.next(Kind::ScreenOpen, 0);
    REQUIRE(o);
    REQUIRE(h.next(Kind::ScreenTree, o->instance));
    h.host->screenEvent(o->instance, {{"kind", "tap"}, {"id", "y"}});
    REQUIRE(h.next(Kind::ScreenTree, o->instance));
    h.host->closeScreen(o->instance);
    std::optional<PluginHostEvent> t;
    REQUIRE(eventually([&] { t = h.next(Kind::ScreenTree, a, 50); return t && Host::firstText(*t).find("result") != std::string::npos; }));
    CHECK(Host::firstText(*t) == "open true,tap true,back false,result b");
}

TEST_CASE("screens host: zk.ui.refresh inside render does nothing; refresh-driven renders count as quiet run time") {
    SUBCASE("inside render") {
        Host h(R"JS(let n = 0; zk.screen("s", { title: "S", render: async () => { n++; await null; zk.ui.refresh(); return zk.ui.text(`n=${n}`); } });)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        auto t = h.next(Kind::ScreenTree, s);
        REQUIRE(t);
        CHECK(Host::firstText(*t) == "n=1");
        CHECK_FALSE(h.next(Kind::ScreenTree, s, 300));
    }
    SUBCASE("a timer calling refresh is throttled") {
        Host h(R"JS(
let n = 0;
const tick = () => { zk.ui.refresh(); setTimeout(tick, 0); };
zk.screen("s", {
  title: "S",
  render: () => { n++; const t = Date.now(); while (Date.now() - t < 10) {} return zk.ui.text(`n=${n}`); },
  onEvent(e) { if (e.kind === "open") setTimeout(tick, 50); },
});
)JS", {}, 300, 600, [](PluginHostDeps& d) { d.quietRunMs = 100; });
        const uint64_t s = h.host->openScreen("p", "s");
        int trees = 0;
        while (trees < 50 && h.next(Kind::ScreenTree, s, 700)) ++trees;  // until none comes for 0.7 s
        CHECK(trees > 1);
        CHECK(trees < 20);
    }
}

TEST_CASE("screens host: instance numbers are unique across hosts (a host made again after an install)") {
    std::set<uint64_t> seen;
    for (int i = 0; i < 2; ++i) {
        Host h(R"JS(zk.screen("s", { title: "S", render: () => zk.ui.text("x") });)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        CHECK(seen.insert(s).second);
        REQUIRE(h.next(Kind::ScreenTree, s));
    }
}

namespace {
const char* kTwoScreens = R"JS(
const seen = [];
zk.screen("a", {
  title: "A",
  render: () => zk.ui.text(seen.join(",") || "-"),
  onEvent: async (e) => {
    if (e.kind === "tap" && e.id === "go") { await null; zk.ui.open("b", { x: 1 }); }
    if (e.kind === "result") seen.push(`result:${e.from}:${JSON.stringify(e.value)}`);
  },
});
zk.screen("b", {
  title: "Bee",
  render: (p) => zk.ui.text(`x=${p.x}`),
  onEvent(e, p) {
    if (e.kind === "back") seen.push("b got back");
    if (e.kind === "tap" && e.id === "done") zk.ui.close(`filed ${p.x}`);
  },
});
)JS";
}  // namespace

TEST_CASE("screens host: zk.ui.open and zk.ui.close round trip; the result reaches the screen below") {
    Host h(kTwoScreens);
    const uint64_t a = h.host->openScreen("p", "a");
    REQUIRE(h.next(Kind::ScreenTree, a));
    h.host->screenEvent(a, {{"kind", "tap"}, {"id", "go"}});
    auto o = h.next(Kind::ScreenOpen, 0);
    REQUIRE(o);
    CHECK(o->parent == a);
    CHECK(o->screenId == "b");
    CHECK(o->text == "Bee");
    CHECK(o->value == json{{"x", 1}});
    const uint64_t b = o->instance;
    auto tb = h.next(Kind::ScreenTree, b);
    REQUIRE(tb);
    CHECK(Host::firstText(*tb) == "x=1");
    REQUIRE(h.next(Kind::ScreenTree, a));  // a renders after its event too

    h.host->screenEvent(b, {{"kind", "tap"}, {"id", "done"}});
    auto c = h.next(Kind::ScreenClose, b);
    REQUIRE(c);
    CHECK(c->value == "filed 1");
    auto ta = h.next(Kind::ScreenTree, a);
    REQUIRE(ta);
    CHECK(Host::firstText(*ta) == "result:b:\"filed 1\"");
    CHECK_FALSE(h.next(Kind::ScreenTree, b, 200));  // b is gone
}

TEST_CASE("screens host: Back closes at once; the plugin hears `back`, then the screen below a `result` without value") {
    Host h(kTwoScreens);
    const uint64_t a = h.host->openScreen("p", "a");
    REQUIRE(h.next(Kind::ScreenTree, a));
    h.host->screenEvent(a, {{"kind", "tap"}, {"id", "go"}});
    auto o = h.next(Kind::ScreenOpen, 0);
    REQUIRE(o);
    REQUIRE(h.next(Kind::ScreenTree, o->instance));
    REQUIRE(h.next(Kind::ScreenTree, a));
    h.host->closeScreen(o->instance);
    auto ta = h.next(Kind::ScreenTree, a);
    REQUIRE(ta);
    CHECK(Host::firstText(*ta) == "b got back,result:b:undefined");
    CHECK_FALSE(h.next(Kind::ScreenTree, o->instance, 200));
    CHECK_FALSE(h.next(Kind::ScreenClose, o->instance, 50));  // the app closed it: no ScreenClose
}

TEST_CASE("screens host: [Review Focus] the plugin turned off or restarted while its screen is open → ScreenError") {
    SUBCASE("turned off") {
        Host h(R"JS(zk.screen("s", { title: "S", render: () => zk.ui.text("x") });)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        REQUIRE(h.next(Kind::ScreenTree, s));
        h.host->setEnabled("p", false);
        auto e = h.next(Kind::ScreenError, s);
        REQUIRE(e);
        CHECK(e->text == "P can't show this screen right now");
    }
    SUBCASE("restarted after its onEvent was stopped mid-run") {
        Host h(R"JS(zk.screen("s", { title: "S", render: () => zk.ui.text("x"), onEvent(e) { if (e.kind === "tap") for (;;) {} } });)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        REQUIRE(h.next(Kind::ScreenTree, s));
        h.host->screenEvent(s, {{"kind", "tap"}, {"id", "x"}});
        REQUIRE(h.next(Kind::ScreenError, s));
        CHECK(h.failures() == 1);
    }
    SUBCASE("removed") {
        Host h(R"JS(zk.screen("s", { title: "S", render: () => zk.ui.text("x") });)JS");
        const uint64_t s = h.host->openScreen("p", "s");
        REQUIRE(h.next(Kind::ScreenTree, s));
        h.host->remove("p");
        REQUIRE(h.next(Kind::ScreenError, s));
    }
}

TEST_CASE("screens host: zk.ui.open only in answer to the user; close only from onEvent; refresh from anywhere") {
    Host h(R"JS(
let n = 0;
const tryIt = (what, f) => { try { f(); console.log(`${what} ALLOWED`); } catch (e) { console.log(`${what} REFUSED ${e.message}`); } };
tryIt("load open", () => zk.ui.open("s"));
zk.on("syncFinished", () => { tryIt("handler open", () => zk.ui.open("s")); tryIt("handler close", () => zk.ui.close()); });
zk.background(async () => { tryIt("background open", () => zk.ui.open("s")); n = 5; zk.ui.refresh(); });
zk.screen("s", {
  title: "S",
  render: () => zk.ui.text(`n=${n}`),
  onEvent(e) { if (e.kind === "tap") tryIt("unknown open", () => zk.ui.open("nope")); },
});
)JS", {"background"});
    const uint64_t s = h.host->openScreen("p", "s");
    REQUIRE(h.next(Kind::ScreenTree, s));
    h.host->dispatch({"syncFinished", {}, true});
    h.host->runBackground();
    auto t = h.next(Kind::ScreenTree, s);
    REQUIRE(t);
    CHECK(Host::firstText(*t) == "n=5");
    h.host->screenEvent(s, {{"kind", "tap"}, {"id", "x"}});
    REQUIRE(h.next(Kind::ScreenTree, s));
    const std::string log = h.log();
    CAPTURE(log);
    for (const char* what : {"load open", "handler open", "handler close", "background open", "unknown open"})
        CHECK(log.find(std::string(what) + " REFUSED") != std::string::npos);
    CHECK(log.find("ALLOWED") == std::string::npos);
    CHECK(log.find("there is no screen \"nope\"") != std::string::npos);
    CHECK_FALSE(h.next(Kind::ScreenOpen, 0, 100));
}

TEST_CASE("screens host: the screen API's arguments are checked") {
    Host h(R"JS(
const bad = [
  () => zk.screen("s"), () => zk.screen("s", () => 1), () => zk.screen("s", { render: () => 1 }),
  () => zk.screen("s", { title: "T" }), () => zk.screen("s", { title: "T", render: () => 1, onEvent: 3 }),
  () => zk.screen("s", { title: "T", render: () => 1, main: "yes" }), () => zk.ui.open(3),
];
for (const f of bad) { try { f(); console.log("ACCEPTED"); } catch (e) { console.log(`${e.name}: ${e.message}`); } }
console.log(JSON.stringify([zk.ui.text("Hi"), zk.ui.space(), zk.ui.buttons([{ id: "a", title: "A" }]), zk.ui.column([], { gap: "large" })]));
)JS");
    h.host->dispatch({"syncFinished", {}, false});
    h.host->waitIdle(3000);
    const std::string log = h.log();
    CAPTURE(log);
    CHECK(log.find("ACCEPTED") == std::string::npos);
    for (const char* m : {"`title`", "`render`", "`onEvent`", "`main`", "zk.ui.open: the screen id"}) CHECK(log.find(m) != std::string::npos);
    CHECK(log.find(R"([{"kind":"text","text":"Hi"},{"kind":"space"},{"kind":"buttons","buttons":[{"id":"a","title":"A"}]},{"kind":"column","children":[],"gap":"large"}])") != std::string::npos);
}
