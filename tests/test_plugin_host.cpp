#include <doctest/doctest.h>

#include <sys/resource.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <sys/syscall.h>
#endif
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include "fake_plugins.h"
#include "plugins/host.h"
#include "plugins/js.h"
#include "plugins/plugin_ctx.h"
#include "plugins/plugin_log.h"
#include "plugins/state.h"

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

const PluginEvent kSync{"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// zk.test.*: host work the test controls, through the same interface later bindings use.
struct TestWork {
    std::mutex mu;
    std::vector<std::shared_ptr<plugins::Work>> hung;
    std::vector<uint64_t> asks;
};
TestWork& testWork() {
    static TestWork w;
    return w;
}

JSValue testHang(JSContext* c, JSValueConst, int, JSValueConst*) {
    JSValue promise;
    auto work = plugins::startWork(plugins::ctxOf(c), &promise);
    std::lock_guard<std::mutex> lock(testWork().mu);
    testWork().hung.push_back(work);
    return promise;
}

JSValue testLater(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    int32_t ms = 0;
    if (argc > 0) JS_ToInt32(c, &ms, argv[0]);
    JSValue promise;
    auto work = plugins::startWork(plugins::ctxOf(c), &promise);
    std::thread([work, ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        work->resolve(nlohmann::json("done"));
    }).detach();
    return promise;
}

JSValue testAsk(JSContext* c, JSValueConst, int, JSValueConst*) {
    auto& p = plugins::ctxOf(c);
    auto holder = std::make_shared<std::shared_ptr<plugins::Work>>();
    PluginHostEvent ev{PluginHostEvent::Kind::Confirm, "", "", "ask", {}, 0, ""};
    uint64_t id = plugins::askUser(p, ev, [holder](bool yes) {
        if (*holder) (*holder)->resolve(nlohmann::json(yes));
    });
    if (id == 0) return JS_NewString(c, "not asked");
    JSValue promise;
    *holder = plugins::startWork(p, &promise);
    std::lock_guard<std::mutex> lock(testWork().mu);
    testWork().asks.push_back(id);
    return promise;
}

JSValue testNativeThrows(JSContext*, JSValueConst, int, JSValueConst*) { throw std::runtime_error("native boom"); }
JSValue testNativeBadAlloc(JSContext*, JSValueConst, int, JSValueConst*) { throw std::bad_alloc(); }

// Host work whose settling throws something that isn't a std::exception: it reaches the entry.
JSValue testPoison(JSContext* c, JSValueConst, int, JSValueConst*) {
    JSValue promise;
    auto work = plugins::startWork(plugins::ctxOf(c), &promise);
    work->finish([](JSContext*) -> JSValue { throw 42; });
    return promise;
}

JSValue testThrowing(JSContext* c, JSValueConst, int, JSValueConst*) {
    JSValue promise;
    auto work = plugins::startWork(plugins::ctxOf(c), &promise);
    work->finish([](JSContext*) -> JSValue { throw std::runtime_error("boom from the host"); });
    return promise;
}

void installTestBindings(plugins::PluginCtx& p) {
    JSContext* c = p.cx.raw();
    JSValue t = JS_NewObject(c);
    plugins::defineFunction<testHang>(c, t, "hang", 0);
    plugins::defineFunction<testLater>(c, t, "later", 1);
    plugins::defineFunction<testAsk>(c, t, "ask", 0);
    plugins::defineFunction<testThrowing>(c, t, "throwing", 0);
    plugins::defineFunction<testNativeThrows>(c, t, "nativeThrows", 0);
    plugins::defineFunction<testNativeBadAlloc>(c, t, "nativeBadAlloc", 0);
    plugins::defineFunction<testPoison>(c, t, "poison", 0);
    JS_SetPropertyStr(c, p.zk.raw(), "test", t);
}

PluginHostDeps withTestBindings(PluginHostDeps d) {
    d.extraBindings.push_back(installTestBindings);
    return d;
}

int failuresOf(const FakePlugins& fp, const std::string& id) {
    PluginState s(fp.path(".state.json"));
    return s.failures(id);
}

}  // namespace

TEST_CASE("host: an enabled plugin loads and its console.log reaches its log") {
    FakePlugins fp;
    fp.add("a", R"(
        console.log("loaded", zk.plugin.id, zk.plugin.version);
        zk.on("syncFinished", ({ initial }) => console.log("synced", initial));
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    auto st = host.status();
    REQUIRE(statusOf(st, "a"));
    CHECK(statusOf(st, "a")->loaded);
    CHECK(statusOf(st, "a")->enabled);
    std::string log = fp.log("a");
    CHECK(contains(log, "loaded a 1.0.0"));
    CHECK(contains(log, "synced false"));
}

TEST_CASE("host: a disabled plugin is not loaded") {
    FakePlugins fp;
    fp.add("a", R"(zk.on("syncFinished", () => console.log("a ran"));)");
    fp.add("b", R"(console.log("b loaded"); zk.on("syncFinished", () => console.log("b ran"));)");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    auto st = host.status();
    CHECK(statusOf(st, "a")->loaded);
    CHECK_FALSE(statusOf(st, "b")->loaded);
    CHECK_FALSE(statusOf(st, "b")->enabled);
    CHECK_FALSE(fp.exists("b/plugin.log"));
    CHECK(contains(fp.log("a"), "a ran"));
}

TEST_CASE("host: a throw at load marks it failed with the stack and the others still load") {
    FakePlugins fp;
    fp.add("a", R"(zk.on("syncFinished", () => console.log("a ran"));)");
    fp.add("b", "function boot() {\n  throw new Error(\"no config\");\n}\nboot();\n");
    fp.add("c", R"(zk.on("syncFinished", () => console.log("c ran"));)");
    PluginHost host(fp.deps());
    for (auto id : {"a", "b", "c"}) host.setEnabled(id, true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    auto st = host.status();
    CHECK(statusOf(st, "a")->loaded);
    CHECK(statusOf(st, "c")->loaded);
    const PluginStatus* b = statusOf(st, "b");
    CHECK_FALSE(b->loaded);
    REQUIRE_FALSE(b->errors.empty());
    CHECK(contains(b->errors.back(), "no config"));
    CHECK(contains(b->errors.back(), "main.js:2"));  // the stack
    CHECK(contains(b->errors.back(), "boot"));
    CHECK(contains(fp.log("b"), "no config"));
    CHECK_FALSE(contains(fp.log("b"), "Uncaught"));  // reported once
    CHECK(contains(fp.log("a"), "a ran"));
    CHECK(contains(fp.log("c"), "c ran"));
}

TEST_CASE("host: an import of another file fails at load and says to bundle") {
    FakePlugins fp;
    fp.add("a", "import { x } from \"./util.js\";\nconsole.log(x);\n");
    fp.write("a/util.js", "export const x = 1;");
    fp.add("b", R"(zk.on("syncFinished", async () => {
        try { await import("./util.js"); } catch (e) { console.log("dynamic:", e.message); }
    });)");
    fp.write("b/util.js", "export const x = 1;");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    auto st = host.status();
    auto* a = statusOf(st, "a");
    CHECK_FALSE(a->loaded);
    REQUIRE_FALSE(a->errors.empty());
    CHECK(contains(a->errors.back(), "util.js"));
    CHECK(contains(a->errors.back(), "bundle"));
    CHECK(contains(a->errors.back(), "one main.js"));
    CHECK(contains(fp.log("b"), "dynamic: main.js imports \"util.js\", but a plugin is one file"));
}

TEST_CASE("host: a handler over budget is stopped, counted, and the next event runs") {
    // [Review Focus] a handler awaiting forever: the budget ends it, its host work is rejected
    // with zk.LimitError (so plugin-side queues move on), and the next event still runs.
    FakePlugins fp;
    fp.add("a", R"(
        let n = 0;
        zk.on("syncFinished", async () => {
          if (++n === 1) {
            try { await zk.test.hang(); }
            catch (e) { console.log("rejected", e instanceof zk.LimitError, e.limit, e.message); }
            return;
          }
          console.log("a second event", n);
        });
    )");
    fp.add("b", R"(
        let n = 0;
        zk.on("syncFinished", async () => {
          if (++n === 1) await new Promise(() => {});
          console.log("b second event", n);
        });
    )");
    { std::lock_guard<std::mutex> lock(testWork().mu); testWork().hung.clear(); }
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    auto t0 = std::chrono::steady_clock::now();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took >= 250);
    CHECK(took < 1500);
    {
        std::lock_guard<std::mutex> lock(testWork().mu);
        REQUIRE(testWork().hung.size() == 1);
        CHECK(testWork().hung[0]->cancelled());
    }
    CHECK(contains(fp.log("a"), "rejected true time"));
    for (std::string id : {"a", "b"}) {
        CAPTURE(id);
        CHECK(failuresOf(fp, id) == 1);
        auto st = host.status();
        auto* s = statusOf(st, id);
        REQUIRE_FALSE(s->errors.empty());
        CHECK(contains(s->errors.back(), "syncFinished handler took longer than 0.3 s"));
        CHECK(s->enabled);
    }

    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "a second event 2"));
    CHECK(contains(fp.log("b"), "b second event 2"));
    CHECK(failuresOf(fp, "a") == 0);  // a successful call resets the count
    CHECK(failuresOf(fp, "b") == 0);
}

TEST_CASE("host: a handler in an infinite loop is stopped at its budget") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", ({ spin, n }) => { if (spin) for (;;) {} console.log("next", n); });
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch({"syncFinished", {{"spin", true}}, false});
    host.dispatch({"syncFinished", {{"n", 2}}, false});
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "next 2"));
    auto st = host.status();
    auto* a = statusOf(st, "a");
    REQUIRE_FALSE(a->errors.empty());
    CHECK(contains(a->errors.back(), "took longer than 0.3 s"));
}

TEST_CASE("host: a memory bomb and runaway recursion are stopped, named, and the plugin runs on") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", ({ n }) => {
          if (n === 1) { const hoard = []; for (;;) hoard.push(new Array(100000).fill(n)); }
          if (n === 2) { const deep = (k) => deep(k + 1) + 1; deep(0); }
          console.log("still here", n);
        });
        zk.on("syncFinished", async () => { await Promise.reject(new TypeError("nobody waits")); });
        zk.on("syncFinished", () => { Promise.reject(new RangeError("fire and forget")); });
    )");
    auto deps = fp.deps();
    deps.memoryPerPlugin = 8 << 20;
    deps.handlerBudgetMs = 5000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    for (int i = 1; i <= 3; ++i) host.dispatch({"syncFinished", {{"n", i}}, false});
    REQUIRE(host.waitIdle(10000));
    std::string log = fp.log("a");
    CHECK(contains(log, "Stopped: the syncFinished handler used more than 8 MB of memory"));
    CHECK(contains(log, "Stopped: the syncFinished handler ran out of stack"));
    CHECK(contains(log, "still here 3"));
    CHECK(contains(log, "The syncFinished handler failed: TypeError: nobody waits"));
    CHECK(contains(log, "Uncaught (in promise) RangeError: fire and forget"));
    CHECK_FALSE(contains(log, "Uncaught (in promise) TypeError"));  // reported once, as the call's failure
}

TEST_CASE("host: recursion through a caught PermissionError still meets the stack limit") {
    // A native that throws must not move the engine's stack check deeper (that would let
    // catch-and-recurse code run past it, off the end of the thread's stack).
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", () => {
          const deep = (k) => {
            if (k % 200 === 0) { try { zk.background(() => {}); } catch (e) { if (!(e instanceof zk.PermissionError)) throw e; } }
            return deep(k + 1) + 1;
          };
          deep(0);
        });
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "ran out of stack"));
}

TEST_CASE("host: a C++ exception in a native becomes a JS Error; bad_alloc is the memory limit") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", ({ n }) => {
          if (n === 1) {
            try { zk.test.nativeThrows(); } catch (e) { console.log("caught", e instanceof Error, e.message); }
          }
          if (n === 2) zk.test.nativeBadAlloc();
          if (n === 3) console.log("still working");
        });
    )");
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.start();
    for (int i = 1; i <= 3; ++i) host.dispatch({"syncFinished", {{"n", i}}, false});
    REQUIRE(host.waitIdle(3000));
    std::string log = fp.log("a");
    CHECK(contains(log, "caught true The app failed doing this: native boom"));
    CHECK(contains(log, "Stopped: the syncFinished handler used more than 32 MB of memory"));
    CHECK(contains(log, "still working"));
    CHECK(failuresOf(fp, "a") == 0);  // the last call succeeded
}

TEST_CASE("host: an exception that reaches the entry counts, drops the call's work and restarts the plugin") {
    FakePlugins fp;
    fp.add("a", R"(
        console.log("main.js evaluated");
        zk.on("syncFinished", async ({ n }) => {
          if (n === 1) {
            setTimeout(() => console.log("timer of the poisoned call"), 100);
            await zk.test.poison();
            console.log("never");
          }
          if (n === 2) console.log("second event ran");
        });
    )");
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.start();
    host.dispatch({"syncFinished", {{"n", 1}}, false});
    REQUIRE(host.waitIdle(3000));
    CHECK(failuresOf(fp, "a") == 1);
    host.dispatch({"syncFinished", {{"n", 2}}, false});
    REQUIRE(host.waitIdle(3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::string log = fp.log("a");
    CHECK(contains(log, "internal error"));
    CHECK(contains(log, "restarted"));
    CHECK(log.find("main.js evaluated") != log.rfind("main.js evaluated"));
    CHECK(contains(log, "second event ran"));
    CHECK_FALSE(contains(log, "never"));
    CHECK_FALSE(contains(log, "timer of the poisoned call"));
}

TEST_CASE("host: a stopped call's cleanup code can set timers, but can't ask the user") {
    // A queue that pauses after a LimitError must move on; asking from a stopped call is refused.
    FakePlugins fp;
    fp.add("a", R"(
        let chain = Promise.resolve();
        const serially = (fn) => (chain = chain.then(fn, fn));
        const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
        zk.on("syncFinished", ({ hang }) => serially(async () => {
          if (hang) {
            try { await zk.test.hang(); }
            catch (e) {
              console.log("ask from a stopped call:", await zk.test.ask());
              await pause(50);
              console.log("paused after", e.limit);
            }
            return;
          }
          console.log("next job ran");
        }));
    )");
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.start();
    host.dispatch({"syncFinished", {{"hang", true}}, false});
    REQUIRE(eventually([&] { return contains(fp.log("a"), "paused after time"); }));
    host.dispatch({"syncFinished", {{"hang", false}}, false});
    REQUIRE(host.waitIdle(3000));
    std::string log = fp.log("a");
    CHECK(contains(log, "ask from a stopped call: not asked"));
    CHECK(contains(log, "next job ran"));
    PluginHostEvent ev;
    while (host.poll(ev)) CHECK(ev.kind != PluginHostEvent::Kind::Confirm);
}

TEST_CASE("host: time waiting for the user's answer doesn't count against the budget") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", async () => {
          const yes = await zk.test.ask();
          console.log("answered", yes);
        });
    )");
    { std::lock_guard<std::mutex> lock(testWork().mu); testWork().asks.clear(); }
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    PluginHostEvent ev;
    REQUIRE(eventually([&] { return host.poll(ev); }));
    CHECK(ev.kind == PluginHostEvent::Kind::Confirm);
    CHECK(ev.pluginId == "a");
    CHECK(ev.pluginName == "A");
    CHECK(ev.requestId != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));  // twice the budget
    CHECK_FALSE(fp.exists(".running"));  // the marker names code that runs, not a call that waits
    host.answer(ev.requestId, true);
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "answered true"));
    CHECK(statusOf(host.status(), "a")->errors.empty());
}

TEST_CASE("host: three failures in a row turn it off and post Disabled with \"<name> stopped working — turned off\"") {
    // An uncaught error and a limit stop count alike.
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", ({ n }) => {
          if (n === 2) for (;;) {}
          throw new Error("bad " + n);
        });
    )", {}, "Feed Riffle");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    for (int i = 1; i <= 3; ++i) host.dispatch({"syncFinished", {{"n", i}}, false});
    REQUIRE(host.waitIdle(3000));
    PluginHostEvent ev;
    bool disabled = false;
    while (host.poll(ev))
        if (ev.kind == PluginHostEvent::Kind::Disabled) {
            disabled = true;
            CHECK(ev.pluginId == "a");
            CHECK(ev.text == "Feed Riffle stopped working — turned off");
        }
    CHECK(disabled);
    auto st = host.status();
    auto* a = statusOf(st, "a");
    CHECK_FALSE(a->enabled);
    CHECK_FALSE(a->loaded);
    CHECK(a->errors.size() == 3);
    std::string log = fp.log("a");
    CHECK(contains(log, "bad 1"));
    CHECK(contains(log, "bad 3"));
    CHECK(contains(log, "Feed Riffle stopped working — turned off after 3 failures in a row"));

    host.dispatch({"syncFinished", {{"n", 4}}, false});  // off: nothing runs
    REQUIRE(host.waitIdle(3000));
    CHECK_FALSE(contains(fp.log("a"), "bad 4"));

    host.setEnabled("a", true);  // back on: failures cleared, loads at the next event
    CHECK(failuresOf(fp, "a") == 0);
    host.dispatch({"syncFinished", {{"n", 5}}, false});
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "bad 5"));
    CHECK(statusOf(host.status(), "a")->loaded);
}

TEST_CASE("host: plugins don't share globals") {
    FakePlugins fp;
    fp.add("a", R"(
        globalThis.shared = "a";
        Array.prototype.evil = 1;
        zk.on("syncFinished", () => console.log("a sees", typeof shared, [].evil));
    )");
    fp.add("b", R"(zk.on("syncFinished", () => console.log("b sees", typeof shared, [].evil));)");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "a sees string 1"));
    CHECK(contains(fp.log("b"), "b sees undefined undefined"));
}

TEST_CASE("host: the crash marker names the plugin whose call never returned and that plugin starts off") {
    FakePlugins fp;
    fp.add("a", R"(zk.on("syncFinished", () => { for (;;) {} });)", {}, "Feed Riffle");
    fp.add("b", R"(zk.on("syncFinished", () => console.log("b ran"));)");
    std::string marker;
    {
        auto deps = fp.deps();
        deps.handlerBudgetMs = 60000;
        PluginHost host(deps);
        host.setEnabled("a", true);
        host.setEnabled("b", true);
        host.start();
        host.dispatch(kSync);
        REQUIRE(eventually([&] { return fp.read(".running") == "a"; }));
        marker = fp.read(".running");
        host.stop();
        CHECK_FALSE(fp.exists(".running"));  // a clean stop leaves no marker
        CHECK(failuresOf(fp, "a") == 0);     // nor a failure
    }
    fp.write(".running", marker);  // as if the app had died there
    PluginHost host(fp.deps());
    CHECK_FALSE(fp.exists(".running"));
    auto st = host.status();
    CHECK_FALSE(statusOf(st, "a")->enabled);
    CHECK(statusOf(st, "b")->enabled);
    REQUIRE_FALSE(statusOf(st, "a")->errors.empty());
    CHECK(statusOf(st, "a")->errors.back() == "The app stopped while Feed Riffle was running — turned off");
    PluginHostEvent ev;
    REQUIRE(host.poll(ev));
    CHECK(ev.kind == PluginHostEvent::Kind::Disabled);
    CHECK(ev.text == "The app stopped while Feed Riffle was running — turned off");
}

TEST_CASE("host: stop() returns within 2 s with a plugin in an infinite loop") {
    FakePlugins fp;
    fp.add("a", R"(zk.on("syncFinished", () => { for (;;) {} });)");
    auto deps = fp.deps();
    deps.handlerBudgetMs = 60000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(eventually([&] { return fp.read(".running") == "a"; }));
    auto t0 = std::chrono::steady_clock::now();
    host.stop();
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(took < 2000);
}

TEST_CASE("host: nothing loads before start(); a plugin with only events or a background task loads at its first event or task") {
    FakePlugins fp;
    fp.add("a", R"(console.log("a loaded"); zk.on("syncFinished", () => console.log("a event"));)");
    fp.add("b", R"(console.log("b loaded"); zk.background(() => console.log("b task"));)", {"background"});

    SUBCASE("an event queued before start() runs after it") {
        PluginHost host(fp.deps());
        host.setEnabled("a", true);
        host.dispatch(kSync);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK_FALSE(statusOf(host.status(), "a")->loaded);
        CHECK_FALSE(fp.exists("a/plugin.log"));
        host.start();
        REQUIRE(host.waitIdle(3000));
        CHECK(statusOf(host.status(), "a")->loaded);
        CHECK(contains(fp.log("a"), "a event"));
    }
    SUBCASE("started, nothing loads until a task is due") {
        PluginHost host(fp.deps());
        host.setEnabled("b", true);
        host.start();
        REQUIRE(host.waitIdle(3000));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(statusOf(host.status(), "b")->loaded);
        CHECK_FALSE(fp.exists("b/plugin.log"));
        host.runBackground();
        REQUIRE(host.waitIdle(3000));
        CHECK(statusOf(host.status(), "b")->loaded);
        std::string log = fp.log("b");
        CHECK(contains(log, "b loaded"));
        CHECK(contains(log, "b task"));
    }
}

TEST_CASE("host: plugins share a 96 MB memory pool; one pushing it over is stopped, the others run on") {
    SUBCASE("many small plugins load fine") {
        FakePlugins fp;
        std::vector<std::string> ids = {"a", "b", "c", "d", "e", "f"};
        for (const auto& id : ids) fp.add(id, R"(zk.on("syncFinished", () => console.log("ran"));)");
        PluginHost host(fp.deps());  // 32 MB each, 96 MB together: usage counts, not reservations
        for (const auto& id : ids) host.setEnabled(id, true);
        host.start();
        host.dispatch(kSync);
        REQUIRE(host.waitIdle(3000));
        auto st = host.status();
        for (const auto& id : ids) {
            CAPTURE(id);
            CHECK(statusOf(st, id)->loaded);
            CHECK(statusOf(st, id)->errors.empty());
            CHECK(contains(fp.log(id), "ran"));
        }
    }
    SUBCASE("the pool, not the plugin's own limit, stops a hog") {
        FakePlugins fp;
        for (auto id : {"a", "b", "c"}) fp.add(id, R"(zk.on("syncFinished", () => console.log("ran")); )");
        fp.add("hog", R"(
            zk.on("syncFinished", ({ hoard }) => {
              if (hoard) { const all = []; for (;;) all.push(new Array(100000).fill(1)); }
              console.log("hog ran");
            });
        )");
        auto deps = fp.deps();
        deps.memoryPerPlugin = 32 << 20;
        deps.memoryTotal = 12 << 20;
        deps.handlerBudgetMs = 5000;
        PluginHost host(deps);
        for (auto id : {"a", "b", "c", "hog"}) host.setEnabled(id, true);
        host.start();
        host.dispatch({"syncFinished", {{"hoard", true}}, false});
        REQUIRE(host.waitIdle(10000));
        CHECK(contains(fp.log("hog"), "Stopped: the syncFinished handler — plugins together used more than 12 MB of memory"));
        host.dispatch(kSync);
        REQUIRE(host.waitIdle(10000));
        auto st = host.status();
        for (auto id : {"a", "b", "c"}) {
            CAPTURE(id);
            CHECK(statusOf(st, id)->errors.empty());
            std::string log = fp.log(id);
            CHECK(log.find("ran") != log.rfind("ran"));  // both events
        }
        CHECK(contains(fp.log("hog"), "restarted"));
        CHECK(contains(fp.log("hog"), "hog ran"));  // the restarted hog runs on too
    }
}

TEST_CASE("host: a stopped call's cleanup that loops is counted, never nests, and others keep running") {
    // [C1] catch the LimitError, start new host work and spin, on every rejection. The cleanup is
    // stopped too and counted; new work from a stopped call's code is rejected at once; the loop
    // keeps serving another plugin; the plugin ends up turned off.
    FakePlugins fp;
    fp.add("a", R"(
        const spin = async () => {
          try { await zk.test.hang(); }
          catch (e) {
            let again = "pending";
            zk.test.hang().catch((e2) => { again = e2 instanceof zk.LimitError && e2.limit; });
            spin();
            await null;
            console.log("new work after the stop:", again);
            for (;;) {}
          }
        };
        zk.on("syncFinished", spin);
    )");
    fp.add("b", R"(let n = 0; zk.on("itemMarkedRead", () => console.log("b event", ++n));)", {"library:read"});
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    host.dispatch(kSync);
    host.dispatch(kSync);  // two calls in flight: both stopped (2), then their cleanup spins (3)
    bool off = false;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 40 && !off; ++i) {
        host.dispatch({"itemMarkedRead", nlohmann::json::object(), false});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        off = !statusOf(host.status(), "a")->enabled;
    }
    auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(off);
    CHECK(took < 1500);  // ~3 budgets of 300 ms
    CHECK(contains(fp.log("a"), "new work after the stop: time"));
    CHECK(contains(fp.log("a"), "(after it was stopped) took longer than 0.3 s"));  // the cleanup's stop, counted
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("b"), "b event 4"));  // served meanwhile
}

TEST_CASE("host: after a stop that interrupted running code the plugin restarts, so its queues work again") {
    // quickjs-ng leaves an interrupted async function's promise pending forever: a module-level
    // queue chained on it would hang; a fresh runtime doesn't.
    FakePlugins fp;
    fp.add("a", R"(
        console.log("main.js evaluated");
        let chain = Promise.resolve();
        const serially = (fn) => (chain = chain.then(fn, fn));
        zk.on("syncFinished", ({ spin }) => serially(async () => {
          if (spin) for (;;) {}
          console.log("queue ran");
        }));
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch({"syncFinished", {{"spin", true}}, false});
    REQUIRE(host.waitIdle(3000));
    host.dispatch({"syncFinished", {{"spin", false}}, false});
    REQUIRE(host.waitIdle(3000));
    std::string log = fp.log("a");
    CHECK(contains(log, "took longer than 0.3 s"));
    CHECK(contains(log, "restarted"));
    CHECK(contains(log, "queue ran"));
    CHECK(log.find("main.js evaluated") != log.rfind("main.js evaluated"));
    CHECK(failuresOf(fp, "a") == 0);  // the stop counted once; the next call's success reset it
}

TEST_CASE("host: bad UTF-8 in a payload and a C++ exception in host work don't stop the thread") {
    FakePlugins fp;
    fp.add("a", R"(
        let n = 0;
        zk.on("syncFinished", async ({ text }) => {
          n++;
          if (n === 1) console.log("text", text === "a\uFFFDb");
          if (n === 2) { try { await zk.test.throwing(); } catch (e) { console.log("caught", e.message); } }
          if (n === 3) console.log("third event ran");
        });
    )");
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.start();
    host.dispatch({"syncFinished", {{"text", std::string("a\xff" "b")}}, false});
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    std::string log = fp.log("a");
    CHECK(contains(log, "text true"));
    CHECK(contains(log, "boom from the host"));
    CHECK(contains(log, "third event ran"));
}

TEST_CASE("host: the background task runs with its own budget and needs its permission") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.background(async () => {
          await zk.test.later(400);  // over the 300 ms event budget, under the 600 ms background one
          console.log("background done");
        });
    )", {"background"});
    fp.add("b", R"(
        try { zk.background(() => {}); }
        catch (e) { console.log(e instanceof zk.PermissionError, e.name, e.permission, e.message); }
    )");
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    host.runBackground();
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "background done"));
    CHECK(statusOf(host.status(), "a")->errors.empty());
    CHECK(contains(fp.log("b"), "true PermissionError background zk.background() needs \"background\" in plugin.json"));
}

TEST_CASE("host: timers run on the plugin thread and are dropped when their call is stopped") {
    FakePlugins fp;
    fp.add("a", R"(
        setTimeout(() => console.log("load timer"), 5);
        let n = 0;
        zk.on("syncFinished", async () => {
          if (++n === 1) {
            const order = [];
            setTimeout(() => order.push("b"), 20);
            setTimeout(() => order.push("a"), 0);
            clearTimeout(setTimeout(() => order.push("never"), 10));
            clearTimeout(12345);
            await new Promise((resolve) => setTimeout(resolve, 50));
            console.log("order", order.join(","));
            return;
          }
          setTimeout(() => console.log("late timer"), 450);
          await new Promise(() => {});
        });
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    CHECK(eventually([&] { return contains(fp.log("a"), "load timer"); }));
    CHECK(contains(fp.log("a"), "order a,b"));
    host.dispatch(kSync);  // stopped at 300 ms; its 450 ms timer is dropped
    REQUIRE(host.waitIdle(3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(contains(fp.log("a"), "late timer"));
    CHECK(contains(fp.log("a"), "took longer than"));
}

TEST_CASE("host: zk.plugin, zk.settings, console formatting and the error classes") {
    FakePlugins fp;
    fp.add("a", R"(
        console.log("plugin", zk.plugin.id, zk.plugin.version, zk.plugin.dataDir.endsWith("/a/data"));
        console.log("settings", zk.settings.apiKey, zk.settings.nested.list.length, Object.isFrozen(zk.settings.nested));
        console.log("n", 1, { a: [1, "x"] }, null, undefined, true);
        console.warn("careful");
        console.error(new Error("boom"));
        try { new zk.PermissionError("x"); } catch (e) { console.log("ctor PermissionError", e instanceof TypeError); }
        try { new zk.LimitError("x"); } catch (e) { console.log("ctor LimitError", e instanceof TypeError); }
        try { zk.on("bookFinished", () => {}); } catch (e) { console.log("on", e.message); }
        try { zk.on("noSuchEvent", () => {}); } catch (e) { console.log("unknown", e instanceof TypeError); }
    )");
    fp.add("b", R"(console.log("settings", JSON.stringify(zk.settings));)");
    fp.write("a/settings.json", R"({"apiKey": "k-123", "nested": {"list": [1, 2, 3]}})");
    fp.write("b/settings.json", "{ not json");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    std::string log = fp.log("a");
    CHECK(contains(log, "plugin a 1.0.0 true"));
    CHECK(contains(log, "settings k-123 3 true"));
    CHECK(contains(log, "n 1 {\"a\":[1,\"x\"]} null undefined true"));
    CHECK(contains(log, "[warn] careful"));
    CHECK(contains(log, "[error] Error: boom"));
    CHECK(contains(log, "ctor PermissionError true"));
    CHECK(contains(log, "ctor LimitError true"));
    CHECK(contains(log, "on zk.on(\"bookFinished\") needs \"library:read\" in plugin.json"));
    CHECK(contains(log, "unknown true"));
    std::string logB = fp.log("b");
    CHECK(contains(logB, "settings {}"));
    CHECK(contains(logB, "settings.json"));  // a warning about the broken file
}

TEST_CASE("host: logs go to the sink too, and rotate at their size limit") {
    FakePlugins fp;
    {
        plugins::PluginLog log(fp.path("p/plugin.log"), 1000, [] { return int64_t(0); });
        std::string line(90, 'x');
        for (int i = 0; i < 30; ++i) log.write(plugins::LogLevel::Info, line);
    }
    CHECK(fs::file_size(fp.path("p/plugin.log")) <= 1000);
    CHECK(fs::file_size(fp.path("p/plugin.log.1")) <= 1000);
    CHECK(fs::file_size(fp.path("p/plugin.log.1")) > 500);

    fp.add("a", R"(console.log("to the sink");)");
    auto deps = fp.deps();
    std::mutex mu;
    std::vector<std::string> lines;
    deps.logSink = [&](const std::string& id, const std::string& line) {
        std::lock_guard<std::mutex> lock(mu);
        lines.push_back(id + ": " + line);
    };
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    host.stop();
    std::lock_guard<std::mutex> lock(mu);
    CHECK(contains(joined(lines), "a: "));
    CHECK(contains(joined(lines), "to the sink"));
}

// ---- Standard globals (URL, URLSearchParams, TextEncoder, TextDecoder) -----------------------

namespace {

// Runs `body` (a module exporting `run()`, which returns a list of failures) in a bare context
// with the standard globals.
std::vector<std::string> runGlobalsSuite(const std::string& body) {
    js::Runtime rt(16 << 20);
    js::Context cx(rt);
    REQUIRE(plugins::installStandardGlobals(cx));
    js::Value ns = cx.evalModule(body, "suite.js");
    auto err = cx.takeError();
    REQUIRE_MESSAGE(!err, (err ? err->message + "\n" + err->stack : ""));
    js::Value r = cx.call(ns.get("run"), {});
    err = cx.takeError();
    REQUIRE_MESSAGE(!err, (err ? err->message + "\n" + err->stack : ""));
    std::vector<std::string> out;
    for (const auto& s : r.toJson()) out.push_back(s.get<std::string>());
    return out;
}

const char* kSuiteHelpers = R"(
const failures = [];
const eq = (what, got, want) => {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g !== w) failures.push(`${what}: got ${g}, want ${w}`);
};
const throws = (what, fn, type) => {
  try { fn(); failures.push(`${what}: did not throw`); }
  catch (e) { if (!(e instanceof type)) failures.push(`${what}: threw ${e}`); }
};
)";

}  // namespace

TEST_CASE("host: URL parses and serialises like the web") {
    auto failures = runGlobalsSuite(std::string(kSuiteHelpers) + R"JS(
export function run() {
  const cases = [
    // [input, base, href or null for failure]
    ["https://export.arxiv.org/api/query", undefined, "https://export.arxiv.org/api/query"],
    ["HTTPS://Example.COM:443/a/./b/../c?x=1#f", undefined, "https://example.com/a/c?x=1#f"],
    ["http://example.com:8080", undefined, "http://example.com:8080/"],
    ["  http://a.b/ c\t\n ", undefined, "http://a.b/%20c"],
    ["../d?q", "https://h/a/b/c", "https://h/a/d?q"],
    ["//other/x", "https://h/a", "https://other/x"],
    ["?y", "https://h/a/b?x#f", "https://h/a/b?y"],
    ["#g", "https://h/a/b?x#f", "https://h/a/b?x#g"],
    ["", "https://h/a?x#y", "https://h/a?x"],
    ["/p", "https://u:p@h:81/a", "https://u:p@h:81/p"],
    ["c", "https://h/a/b", "https://h/a/c"],
    [".", "https://h/a/b", "https://h/a/"],
    ["https://user:pa ss@h/", undefined, "https://user:pa%20ss@h/"],
    ["http://[::1]:80/", undefined, "http://[::1]/"],
    ["http://[2001:db8:0:0:0:0:2:1]/", undefined, "http://[2001:db8::2:1]/"],
    ["http://[::ffff:192.168.0.1]/", undefined, "http://[::ffff:c0a8:1]/"],
    ["http://0x7f.1/", undefined, "http://127.0.0.1/"],
    ["http://192.168.0.257/", undefined, null],
    ["http://ex ample.com/", undefined, null],
    ["https:///", undefined, null],
    ["http:////example.com/x", undefined, "http://example.com/x"],
    ["mailto:someone@example.com", undefined, "mailto:someone@example.com"],
    ["data:text/plain,hi there", undefined, "data:text/plain,hi there"],
    ["foo://host/a b", undefined, "foo://host/a%20b"],
    ["https://münchen.de/", undefined, "https://xn--mnchen-3ya.de/"],
    ["https://Bücher.example/x", undefined, "https://xn--bcher-kva.example/x"],
    ["https://h/?q=é", undefined, "https://h/?q=%C3%A9"],
    ["https://h/é", undefined, "https://h/%C3%A9"],
    ["https://h/#a b", undefined, "https://h/#a%20b"],
    ["https://h/a?b'c", undefined, "https://h/a?b%27c"],
    ["foo://h/a?b'c", undefined, "foo://h/a?b'c"],
    ["a", "mailto:x", null],
    ["file:///home/user/x.txt", undefined, "file:///home/user/x.txt"],
    ["HTTP://EXAMPLE.com/%7Efoo", undefined, "http://example.com/%7Efoo"],
    ["https://h/a/%2e%2E/b", undefined, "https://h/b"],
    ["https://h\\a\\b", undefined, "https://h/a/b"],
    ["https://h:65536/", undefined, null],
    ["https://h:/", undefined, "https://h/"],
    ["http://example.com/foo/../../..", undefined, "http://example.com/"],
    ["https://h/?", undefined, "https://h/?"],
    ["relative/path", undefined, null],
    ["https://doi.org/10.1000/xyz(1)", undefined, "https://doi.org/10.1000/xyz(1)"],
  ];
  for (const [input, base, want] of cases) {
    let got;
    try { got = new URL(input, base).href; } catch (e) { got = e instanceof TypeError ? null : String(e); }
    eq(`new URL(${JSON.stringify(input)}, ${JSON.stringify(base)})`, got, want);
    eq(`URL.canParse(${JSON.stringify(input)})`, URL.canParse(input, base), want !== null);
  }

  const u = new URL("https://user:pw@Example.org:8443/a/b.html?x=1&y=2#top");
  eq("origin", u.origin, "https://example.org:8443");
  eq("protocol", u.protocol, "https:");
  eq("username", u.username, "user");
  eq("password", u.password, "pw");
  eq("host", u.host, "example.org:8443");
  eq("hostname", u.hostname, "example.org");
  eq("port", u.port, "8443");
  eq("pathname", u.pathname, "/a/b.html");
  eq("search", u.search, "?x=1&y=2");
  eq("hash", u.hash, "#top");
  eq("toString", String(u), u.href);
  eq("toJSON", JSON.stringify({ u }), `{"u":"${u.href}"}`);
  eq("base as URL", new URL("c", u).href, "https://user:pw@example.org:8443/a/c");
  eq("mailto origin", new URL("mailto:x@y").origin, "null");
  eq("mailto pathname", new URL("mailto:x@y").pathname, "x@y");

  const s = new URL("https://h/p");
  s.pathname = "/a b/c";
  s.search = "q=1";
  s.hash = "frag";
  s.port = "8080";
  s.hostname = "Other.Example";
  eq("setters", s.href, "https://other.example:8080/a%20b/c?q=1#frag");
  s.port = "443";
  eq("default port dropped", s.href, "https://other.example/a%20b/c?q=1#frag");
  s.host = "h2:99";
  s.protocol = "http";
  s.username = "me";
  s.password = "x y";
  eq("more setters", s.href, "http://me:x%20y@h2:99/a%20b/c?q=1#frag");
  s.protocol = "mailto";  // special to non-special: ignored
  eq("protocol switch ignored", s.protocol, "http:");
  s.hash = "";
  s.search = "";
  eq("cleared", s.href, "http://me:x%20y@h2:99/a%20b/c");
  s.href = "https://new.example/";
  eq("href setter", s.host, "new.example");
  throws("bad href setter", () => { s.href = "nope"; }, TypeError);
  throws("bad url", () => new URL("http://a b"), TypeError);
  throws("no base", () => new URL("/x"), TypeError);

  const arxiv = new URL("https://export.arxiv.org/api/query");
  arxiv.searchParams.set("id_list", ["2401.01234", "2402.5"].join(","));
  arxiv.searchParams.append("max_results", "50");
  eq("searchParams updates href", arxiv.href, "https://export.arxiv.org/api/query?id_list=2401.01234%2C2402.5&max_results=50");
  arxiv.search = "?a=1";
  eq("search updates searchParams", arxiv.searchParams.get("a"), "1");
  eq("same searchParams object", arxiv.searchParams === arxiv.searchParams, true);
  arxiv.searchParams.delete("a");
  eq("empty params drop the ?", arxiv.href, "https://export.arxiv.org/api/query");
  return failures;
}
)JS");
    CHECK_MESSAGE(failures.empty(), joined(failures));
}

TEST_CASE("host: URLSearchParams reads and writes query strings like the web") {
    auto failures = runGlobalsSuite(std::string(kSuiteHelpers) + R"JS(
export function run() {
  const p = new URLSearchParams("?a=1&b=2&a=3");
  eq("getAll", p.getAll("a"), ["1", "3"]);
  eq("get", p.get("a"), "1");
  eq("get missing", p.get("c"), null);
  eq("has", p.has("b"), true);
  eq("has value", p.has("a", "3"), true);
  eq("has wrong value", p.has("a", "4"), false);
  eq("size", p.size, 3);
  eq("toString", p.toString(), "a=1&b=2&a=3");
  p.set("a", "x");
  eq("set replaces all", p.toString(), "a=x&b=2");
  p.append("a", "y");
  p.delete("a", "x");
  eq("delete by value", p.toString(), "b=2&a=y");
  p.delete("b");
  eq("delete", p.toString(), "a=y");

  const q = new URLSearchParams({ search_query: "cat:math.PR", max_results: "50" });
  eq("record + encoding", `${q}`, "search_query=cat%3Amath.PR&max_results=50");
  eq("plus and %20", new URLSearchParams("q=a+b%20c").get("q"), "a b c");
  const r = new URLSearchParams();
  r.set("q", "x y&z=é~*'()!");
  eq("form encoding", r.toString(), "q=x+y%26z%3D%C3%A9%7E*%27%28%29%21");
  eq("bad escapes stay", new URLSearchParams("x=%zz%4").get("x"), "%zz%4");
  eq("utf-8 escapes", new URLSearchParams("x=%C3%A9").get("x"), "é");
  eq("invalid utf-8", new URLSearchParams("x=%FF").get("x"), "\uFFFD");
  eq("no value", new URLSearchParams("a&b=").toString(), "a=&b=");
  eq("pairs", new URLSearchParams([["a", "1"], ["b", "2"]]).toString(), "a=1&b=2");
  throws("bad pair", () => new URLSearchParams([["a"]]), TypeError);
  eq("copy", new URLSearchParams(new URLSearchParams("a=1")).toString(), "a=1");

  const s = new URLSearchParams("c=1&a=2&b=3&a=1");
  s.sort();
  eq("sort is stable", s.toString(), "a=2&a=1&b=3&c=1");
  eq("iterator", [...s], [["a", "2"], ["a", "1"], ["b", "3"], ["c", "1"]]);
  eq("keys", [...s.keys()], ["a", "a", "b", "c"]);
  eq("values", [...s.values()], ["2", "1", "3", "1"]);
  eq("entries", [...s.entries()].length, 4);
  const seen = [];
  s.forEach((v, k, self) => seen.push(k + v + (self === s)));
  eq("forEach", seen, ["a2true", "a1true", "b3true", "c1true"]);
  return failures;
}
)JS");
    CHECK_MESSAGE(failures.empty(), joined(failures));
}

TEST_CASE("host: TextEncoder and TextDecoder speak UTF-8 like the web") {
    auto failures = runGlobalsSuite(std::string(kSuiteHelpers) + R"JS(
const bytes = (a) => Array.from(a);
export function run() {
  const enc = new TextEncoder();
  eq("encoding", enc.encoding, "utf-8");
  eq("ascii", bytes(enc.encode("hi")), [104, 105]);
  eq("bmp + astral", bytes(enc.encode("é€𝄞")), [0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9D, 0x84, 0x9E]);
  eq("lone surrogate", bytes(enc.encode("a\uD800b")), [0x61, 0xEF, 0xBF, 0xBD, 0x62]);
  eq("empty", bytes(enc.encode()), []);
  eq("is Uint8Array", enc.encode("x") instanceof Uint8Array, true);
  eq("NUL kept", bytes(enc.encode("a\0b")), [0x61, 0, 0x62]);

  const dec = new TextDecoder();
  eq("decoder encoding", dec.encoding, "utf-8");
  eq("fatal default", dec.fatal, false);
  eq("ignoreBOM default", dec.ignoreBOM, false);
  eq("round trip", dec.decode(enc.encode("héllo €𝄞")), "héllo €𝄞");
  eq("invalid byte", dec.decode(new Uint8Array([0x61, 0xFF, 0x62])), "a\uFFFDb");
  eq("truncated", dec.decode(new Uint8Array([0xE2, 0x82])), "\uFFFD");
  eq("overlong", dec.decode(new Uint8Array([0xF0, 0x80, 0x80])), "\uFFFD\uFFFD\uFFFD");
  eq("surrogate bytes", dec.decode(new Uint8Array([0xED, 0xA0, 0x80])), "\uFFFD\uFFFD\uFFFD");
  eq("bom stripped", dec.decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41])), "A");
  eq("bom kept", new TextDecoder("utf-8", { ignoreBOM: true }).decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41])), "\uFEFFA");
  eq("ArrayBuffer", dec.decode(enc.encode("ab").buffer), "ab");
  const big = enc.encode("xabcx");
  eq("view with offset", dec.decode(new Uint8Array(big.buffer, 1, 3)), "abc");
  eq("DataView", dec.decode(new DataView(big.buffer, 1, 2)), "ab");
  eq("undefined", dec.decode(), "");
  eq("label utf8", new TextDecoder(" UTF8 ").encoding, "utf-8");
  throws("other encodings", () => new TextDecoder("latin1"), RangeError);
  throws("fatal", () => new TextDecoder("utf-8", { fatal: true }).decode(new Uint8Array([0xFF])), TypeError);
  throws("not bytes", () => dec.decode("text"), TypeError);

  const s = new TextDecoder();
  let out = s.decode(new Uint8Array([0xE2, 0x82]), { stream: true });
  out += s.decode(new Uint8Array([0xAC, 0x41]), { stream: true });
  out += s.decode();
  eq("streaming", out, "€A");
  const t = new TextDecoder();
  eq("stream end flushes", t.decode(new Uint8Array([0xE2]), { stream: true }) + t.decode(), "\uFFFD");
  return failures;
}
)JS");
    CHECK_MESSAGE(failures.empty(), joined(failures));
}

TEST_CASE("host: a plugin that can't get memory for its runtime fails to load; the host runs on") {
    FakePlugins fp;
    fp.add("a", R"(zk.on("syncFinished", () => console.log("ran"));)");
    auto deps = fp.deps();
    deps.memoryTotal = 64;  // too little for any JavaScript runtime
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    auto st = host.status();
    CHECK_FALSE(statusOf(st, "a")->loaded);
    CHECK(contains(statusOf(st, "a")->problem, "not enough plugin memory"));
    CHECK_FALSE(contains(fp.log("a"), "ran"));
}

namespace {
int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}

// A timer chain nobody waits for: each tick runs 50 ms of JavaScript and sets the next.
const char* kBusyChain = R"(
    const busy = (ms) => { const end = Date.now() + ms; while (Date.now() < end) {} };
    const tick = () => { busy(50); console.log("tick"); setTimeout(tick, 0); };
    zk.on("syncFinished", () => { setTimeout(tick, 0); });
    zk.on("bookOpened", async () => {
      const t0 = Date.now();
      for (let i = 0; i < 3; i++) { busy(40); await new Promise((r) => setTimeout(r, 0)); }
      console.log("user call done in", Date.now() - t0);
    });
)";
}  // namespace

TEST_CASE("host: by default a plugin's quiet run time is 2 s per rolling minute") {
    PluginHostDeps d;
    CHECK(d.quietRunMs == 2000);
    CHECK(d.quietWindowMs == 60000);
}

TEST_CASE("host: an orphan timer chain gets at most its quiet run time per window, deferred, not failed") {
    FakePlugins fp;
    fp.add("a", kBusyChain, {"library:read"});
    auto deps = fp.deps();
    deps.quietRunMs = 200;  // 200 ms of JavaScript per 1 s window (the app: 2 s per minute)
    deps.quietWindowMs = 1000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const int ticks = count(fp.log("a"), "tick");
    // Unthrottled: ~50 ticks. Throttled: 200 ms (+ one 50 ms tick over) in each of 2.5 windows.
    CHECK(ticks >= 3);
    CHECK(ticks <= 16);
    auto st = host.status();
    CHECK(statusOf(st, "a")->errors.empty());  // deferred, not failed
    CHECK(failuresOf(fp, "a") == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CHECK(count(fp.log("a"), "tick") > ticks);  // and it goes on when the window allows
}

TEST_CASE("host: a user-caused call isn't throttled by the quiet run time") {
    FakePlugins fp;
    fp.add("a", kBusyChain, {"library:read"});
    auto deps = fp.deps();
    deps.quietRunMs = 100;
    deps.quietWindowMs = 60000;  // the chain is out of quiet time for the rest of the test
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(eventually([&] { return count(fp.log("a"), "tick") >= 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int ticks = count(fp.log("a"), "tick");
    CHECK(ticks <= 3);  // throttled
    host.dispatch({"bookOpened", nlohmann::json::object(), true});
    REQUIRE(eventually([&] { return contains(fp.log("a"), "user call done"); }, 2000));
    CHECK(statusOf(host.status(), "a")->errors.empty());
}

TEST_CASE("host: a paused call's timer entries share its budget; only waiting is free") {
    FakePlugins fp;
    fp.add("a", R"(
        const busy = (ms) => { const end = Date.now() + ms; while (Date.now() < end) {} };
        zk.on("syncFinished", async () => {
          zk.test.ask();  // paused until answered, which never comes
          for (;;) { busy(50); console.log("paused tick"); await new Promise((r) => setTimeout(r, 0)); }
        });
    )");
    PluginHost host(withTestBindings(fp.deps()));  // a 300 ms budget
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(eventually([&] { return contains(fp.log("a"), "took longer than"); }, 3000));
    CHECK(count(fp.log("a"), "paused tick") <= 8);
}

TEST_CASE("host: a user-caused call may ask at most 3 questions; more are not asked") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("bookOpened", () => {
          for (let i = 1; i <= 5; i++) {
            const r = zk.test.ask();
            console.log("question", i, typeof r === "string" ? r : "asked");
          }
        });
    )", {"library:read"});
    PluginHost host(withTestBindings(fp.deps()));
    host.setEnabled("a", true);
    host.start();
    host.dispatch({"bookOpened", nlohmann::json::object(), true});
    REQUIRE(eventually([&] { return contains(fp.log("a"), "question 5"); }));
    int confirms = 0;
    PluginHostEvent ev;
    REQUIRE(eventually([&] {
        while (host.poll(ev)) confirms += ev.kind == PluginHostEvent::Kind::Confirm;
        return confirms >= 3;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    while (host.poll(ev)) confirms += ev.kind == PluginHostEvent::Kind::Confirm;
    CHECK(confirms == 3);
    const std::string log = fp.log("a");
    CHECK(contains(log, "question 3 asked"));
    CHECK(contains(log, "question 4 not asked"));
    CHECK(contains(log, "question 5 not asked"));
    CHECK(contains(log, "3 questions"));
}

namespace {
// The calling thread's scheduling level: its nice value (Linux) or QoS class (macOS).
int threadLevel() {
#if defined(__APPLE__)
    int policy = 0;
    sched_param param{};
    pthread_getschedparam(pthread_self(), &policy, &param);
    return param.sched_priority;
#else
    return getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)));
#endif
}
#if defined(__APPLE__)
const int kLowered = 31 - 10;  // the default priority, 10 lower
#else
const int kLowered = 10;
#endif
std::atomic<int> gPluginThreadLevel{-1000};
JSValue testLevel(JSContext*, JSValueConst, int, JSValueConst*) {
    gPluginThreadLevel = threadLevel();
    return JS_UNDEFINED;
}
}  // namespace

TEST_CASE("host: the plugin thread and its workers run at a lower priority than the app") {
    int worker = -1000;
    std::thread([&] {
        plugins::lowerThreadPriority();
        worker = threadLevel();
    }).join();
    CHECK(worker == kLowered);
    CHECK(threadLevel() != kLowered);  // the calling thread is untouched

    FakePlugins fp;
    fp.add("a", R"(zk.on("syncFinished", () => zk.test.level());)");
    auto deps = fp.deps();
    deps.extraBindings.push_back([](plugins::PluginCtx& p) {
        JSContext* c = p.cx.raw();
        JSValue t = JS_NewObject(c);
        plugins::defineFunction<testLevel>(c, t, "level", 0);
        JS_SetPropertyStr(c, p.zk.raw(), "test", t);
    });
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    CHECK(gPluginThreadLevel.load() == kLowered);
}

TEST_CASE("host: anyEnabled says whether some installed plugin is on") {
    FakePlugins fp;
    fp.add("a", "");
    fp.add("b", "");
    PluginHost host(fp.deps());
    CHECK_FALSE(host.anyEnabled());
    host.setEnabled("b", true);
    CHECK(eventually([&] { return host.anyEnabled(); }));
    host.setEnabled("b", false);
    CHECK(eventually([&] { return !host.anyEnabled(); }));
}

// ---- Memory and battery limits on timers and handlers (audits 2026-11-10) ----------------------

TEST_CASE("host: a plugin may have at most 1000 timers pending; one more throws a LimitError") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", () => {
          const ids = [];
          for (let i = 0; i < 1000; i++) ids.push(setTimeout(() => {}, 1e9));
          try {
            setTimeout(() => {}, 1e9);
            console.log("no limit");
          } catch (e) {
            console.log("caught", e instanceof zk.LimitError, e.limit, e.message);
          }
          clearTimeout(ids[0]);
          setTimeout(() => {}, 1e9);  // room again
          console.log("set after clear");
        });
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    const std::string log = fp.log("a");
    CHECK(contains(log, "caught true timers setTimeout: this plugin already has 1000 timers pending"));
    CHECK_FALSE(contains(log, "no limit"));
    CHECK(contains(log, "set after clear"));
    CHECK(plugins::kMaxTimers == 1000);
}

TEST_CASE("host: a plugin may register at most 100 handlers per event; one more throws a LimitError") {
    FakePlugins fp;
    fp.add("a", R"(
        let calls = 0;
        for (let i = 0; i < 99; i++) zk.on("syncFinished", () => { calls++; });
        zk.on("syncFinished", () => { if (calls === 99) console.log("ran", calls + 1); });
        try {
          zk.on("syncFinished", () => {});
          console.log("no limit");
        } catch (e) {
          console.log("caught", e instanceof zk.LimitError, e.limit, e.message);
        }
        zk.on("bookOpened", () => {});  // another event has its own 100
        console.log("other event ok");
    )", {"library:read"});
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(3000));
    const std::string log = fp.log("a");
    CHECK(contains(log, "caught true handlers zk.on(\"syncFinished\"): this plugin already has 100 handlers"));
    CHECK_FALSE(contains(log, "no limit"));
    CHECK(contains(log, "other event ok"));
    CHECK(contains(log, "ran 100"));  // the 100 registered ones run, not the refused ones
}

TEST_CASE("host: setTimeout has a floor of 100 ms") {
    CHECK(PluginHostDeps{}.minTimerMs == 100);
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", async () => {
          for (const ms of [0, undefined, -5, NaN, 30]) {
            const t0 = Date.now();
            await new Promise((r) => setTimeout(r, ms));
            console.log("waited", String(ms), Date.now() - t0 >= 99);
          }
        });
    )");
    auto deps = fp.deps();
    deps.minTimerMs = 100;
    deps.handlerBudgetMs = 2000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kSync);
    REQUIRE(host.waitIdle(5000));
    const std::string log = fp.log("a");
    for (const char* ms : {"0", "undefined", "-5", "NaN", "30"})
        CHECK_MESSAGE(contains(log, std::string("waited ") + ms + " true"), log);
}

TEST_CASE("host: in the background, timers of ended calls wait for the foreground; a running call's fire") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", async () => {
          setTimeout(() => console.log("orphan fired"), 150);  // due after the handler has ended
          await new Promise((r) => setTimeout(r, 20));
          console.log("in-call timer fired");
        });
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.setBackground(true);
    host.dispatch(kSync);
    REQUIRE(eventually([&] { return contains(fp.log("a"), "in-call timer fired"); }));
    // The handler has ended by now; its other timer was due long ago, but waits.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(contains(fp.log("a"), "orphan fired"));
    host.setBackground(false);
    CHECK(eventually([&] { return contains(fp.log("a"), "orphan fired"); }));
}
