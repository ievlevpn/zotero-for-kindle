// Actions on the plugin thread (PluginHost::runAction/stopAction; zk.selectionAction,
// zk.itemAction): what run gets and returns, re-runs with `again`, errors, stopping, zk.ui.open
// from a run, and the entry version the app watches.

#include <doctest/doctest.h>

#include <chrono>
#include <deque>
#include <thread>

#include "fake_plugins.h"
#include "plugins/host.h"
#include "plugins/plugin_ctx.h"

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;
using Kind = PluginHostEvent::Kind;

namespace {

JSValue jsUserCaused(JSContext* c, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(c, plugins::currentCall(plugins::ctxOf(c)).userCaused);
}

struct Host {
    FakePlugins fp;
    std::unique_ptr<PluginHost> host;
    std::deque<PluginHostEvent> got;

    explicit Host(const std::string& js, int actionMs = 600) {
        fp.add("p", js);
        PluginHostDeps d = fp.deps();
        d.actionBudgetMs = actionMs;
        d.extraBindings.push_back([](plugins::PluginCtx& p) {
            plugins::defineFunction<jsUserCaused>(p.cx.raw(), p.zk.raw(), "userCaused", 0);
        });
        host = std::make_unique<PluginHost>(d);
        host->setEnabled("p", true);
        host->start();
    }
    ~Host() { host->stop(); }

    // The next ActionResult/ActionError for `run` (or a ScreenOpen with run 0), or nullopt after `ms`.
    std::optional<PluginHostEvent> next(uint64_t run, int ms = 3000, Kind only = Kind::Log) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        for (;;) {
            PluginHostEvent ev;
            while (host->poll(ev)) got.push_back(std::move(ev));
            for (auto it = got.begin(); it != got.end(); ++it) {
                const bool action = it->kind == Kind::ActionResult || it->kind == Kind::ActionError;
                if ((only == Kind::Log && action && it->requestId == run) || (only != Kind::Log && it->kind == only)) {
                    PluginHostEvent out = std::move(*it);
                    got.erase(it);
                    return out;
                }
            }
            if (std::chrono::steady_clock::now() >= end) return std::nullopt;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    int failures() const { return statusOf(host->status(), "p")->failures; }
};

const char* kExplain = R"JS(
zk.selectionAction({
  id: "explain",
  title: "Explain",
  run: async (sel, again) => {
    await null;
    if (again) return again.action === "note" ? `saved: ${again.text}` : undefined;
    return { text: `${sel.text} on ${sel.pageLabel} of ${sel.item.title} (${zk.userCaused()})`,
             actions: [{ id: "note", title: "Save as note" }, { id: "close", title: "Done" }] };
  },
});
zk.itemAction({ id: "count", title: "Count", run: (item) => `${item.key} has ${item.tags.length} tags` });
zk.itemAction({ id: "nothing", title: "Nothing", run: () => {} });
zk.itemAction({ id: "throws", title: "Throws", run: () => { throw new Error("no network"); } });
zk.itemAction({ id: "slow", title: "Slow", run: () => { for (;;) {} } });
zk.itemAction({ id: "waits", title: "Waits", run: () => new Promise(() => {}) });
zk.itemAction({ id: "three", title: "Three", run: () => ({ text: "x", actions: [{ id: "a", title: "A" }, { id: "b", title: "B" }, { id: "c", title: "C" }] }) });
zk.itemAction({ id: "number", title: "Number", run: () => 5 });
zk.itemAction({ id: "opens", title: "Opens", run: () => { zk.ui.open("detail", { n: 1 }); } });
zk.screen("detail", { title: "Detail", render: (p) => zk.ui.text(`n=${p.n}`) });
)JS";

nlohmann::ordered_json selection() {
    return {{"text", "a truth"}, {"item", {{"key", "PRIDE001"}, {"title", "Pride"}}}, {"attachment", {{"key", "ATT1"}}}, {"pageLabel", "12"}};
}

}  // namespace

TEST_CASE("actions host: run gets the selection, user-caused; returns text and buttons; again re-runs with {action, text}") {
    Host h(kExplain);
    const uint64_t r = h.host->runAction("p", "selectionAction", "explain", selection());
    auto ev = h.next(r);
    REQUIRE(ev);
    REQUIRE(ev->kind == Kind::ActionResult);
    CHECK(ev->pluginName == "P");
    CHECK(ev->value["text"] == "a truth on 12 of Pride (true)");
    REQUIRE(ev->value["actions"].size() == 2);
    CHECK(ev->value["actions"][0] == json{{"id", "note"}, {"title", "Save as note"}});

    const uint64_t r2 = h.host->runAction("p", "selectionAction", "explain", selection(), {{"action", "note"}, {"text", "shown"}});
    CHECK(r2 != r);
    ev = h.next(r2);
    REQUIRE(ev);
    CHECK(ev->value["text"] == "saved: shown");
    CHECK(ev->value["actions"].empty());

    ev = h.next(h.host->runAction("p", "selectionAction", "explain", selection(), {{"action", "close"}, {"text", "x"}}));
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionResult);
    CHECK(ev->value.is_null());  // nothing returned: no pop-up

    const auto all = h.host->status();
    const auto st = statusOf(all, "p");
    REQUIRE(st);
    CHECK(st->entries.size() == 10);
}

TEST_CASE("actions host: an item action; a string; nothing") {
    Host h(kExplain);
    auto ev = h.next(h.host->runAction("p", "itemAction", "count", {{"key", "K1"}, {"tags", {"a", "b"}}}));
    REQUIRE(ev);
    CHECK(ev->value["text"] == "K1 has 2 tags");
    ev = h.next(h.host->runAction("p", "itemAction", "nothing", {{"key", "K1"}}));
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionResult);
    CHECK(ev->value.is_null());
}

TEST_CASE("actions host: a thrown error, over budget, or what can't be shown → ActionError \"<Plugin> couldn't do that: …\"") {
    Host h(kExplain, 300);
    auto ev = h.next(h.host->runAction("p", "itemAction", "throws", {{"key", "K1"}}));
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionError);
    CHECK(ev->text == "P couldn't do that: no network");
    ev = h.next(h.host->runAction("p", "itemAction", "three", {{"key", "K1"}}));
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionError);
    CHECK(ev->text.find("P couldn't do that: it returned what can't be shown") == 0);
    ev = h.next(h.host->runAction("p", "itemAction", "number", {{"key", "K1"}}));
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionError);
    ev = h.next(h.host->runAction("p", "itemAction", "slow", {{"key", "K1"}}));
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionError);
    CHECK(ev->text == "P couldn't do that: it took longer than 0.3 s");
    ev = h.next(h.host->runAction("p", "itemAction", "unknown", {{"key", "K1"}}));
    REQUIRE(ev);
    CHECK(ev->text == "P couldn't do that: it isn't running");
}

TEST_CASE("actions host: stopAction stops the run (nothing more comes, not a failure); the plugin turned off answers with an error") {
    Host h(kExplain, 5000);
    const uint64_t r = h.host->runAction("p", "itemAction", "waits", {{"key", "K1"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h.host->stopAction(r);
    CHECK_FALSE(h.next(r, 400));
    CHECK(h.host->waitIdle(2000));
    CHECK(h.failures() == 0);
    CHECK(h.fp.log("p").find("the user closed its pop-up") != std::string::npos);

    const uint64_t r2 = h.host->runAction("p", "itemAction", "waits", {{"key", "K1"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h.host->setEnabled("p", false);
    auto ev = h.next(r2);
    REQUIRE(ev);
    CHECK(ev->kind == Kind::ActionError);
    CHECK(ev->text == "P couldn't do that: it stopped");
}

TEST_CASE("actions host: zk.ui.open from a run opens the screen (parent 0) when the run ends") {
    Host h(kExplain);
    const uint64_t r = h.host->runAction("p", "itemAction", "opens", {{"key", "K1"}});
    auto ev = h.next(r);
    REQUIRE(ev);
    CHECK(ev->value.is_null());
    auto open = h.next(0, 3000, Kind::ScreenOpen);
    REQUIRE(open);
    CHECK(open->parent == 0);
    CHECK(open->screenId == "detail");
    CHECK(open->text == "Detail");
    CHECK(open->value == json{{"n", 1}});
    auto tree = h.next(0, 3000, Kind::ScreenTree);
    REQUIRE(tree);
    CHECK(tree->instance == open->instance);
}

TEST_CASE("actions host: wake loads enabled plugins; the entries version moves with entries and badges") {
    FakePlugins fp;
    fp.add("p", R"JS(zk.itemAction({ id: "a", title: "A", run: () => "x" }); zk.setBadge("2 new");)JS");
    PluginHost host(fp.deps());
    host.setEnabled("p", true);
    host.start();
    const uint64_t v0 = host.entriesVersion();
    CHECK(statusOf(host.status(), "p")->entries.empty());  // lazily: not loaded yet
    host.wake();
    REQUIRE(eventually([&] { return statusOf(host.status(), "p")->entries.size() == 1; }));
    CHECK(eventually([&] { return statusOf(host.status(), "p")->badge == "2 new"; }));
    CHECK(host.entriesVersion() > v0);
    const uint64_t v1 = host.entriesVersion();
    host.setEnabled("p", false);
    CHECK(host.entriesVersion() > v1);
    host.stop();
}
