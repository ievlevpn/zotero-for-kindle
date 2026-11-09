#include <doctest/doctest.h>

#include <filesystem>

#include "fake_plugins.h"
#include "plugins/host.h"
#include "plugins/plugin_ctx.h"

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

const PluginEvent kRun{"syncFinished", {{"origin", "zotero"}, {"initial", false}}, false};
const PluginEvent kSecond{"syncFinished", {{"origin", "zotero"}, {"initial", true}}, false};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
size_t where(const std::string& hay, const std::string& needle) { return hay.find(needle); }

// A plugin whose syncFinished handler runs `body` (an async function body); `out(x)` logs "OUT <json>",
// a thrown Error is logged as "ERR <message>". `initial` is true in the second kind of event.
std::string pluginWith(const std::string& body) {
    return "const out = (x) => console.log('OUT ' + (typeof x === 'string' ? x : x === undefined ? 'undefined' : JSON.stringify(x)));\n"
           "zk.on('syncFinished', async ({ initial }) => { try {\n" + body + "\n} catch (e) { console.log('ERR ' + e.message); } });\n";
}

// Runs the handler once and returns the plugin's log.
std::string runOnce(FakePlugins& fp, const std::string& id, const std::string& body) {
    fp.add(id, pluginWith(body));
    PluginHost host(fp.deps());
    host.setEnabled(id, true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    return fp.log(id);
}

}  // namespace

TEST_CASE("storage: set/get/delete/keys round-trip JSON values") {
    FakePlugins fp;
    std::string log = runOnce(fp, "a", R"(
        await zk.storage.set("n", 5);
        await zk.storage.set("o", { a: [1, 2, { b: null }], s: "héllo \u{1F600}" });
        await zk.storage.set("null", null);
        out(await zk.storage.get("n"));
        const o = await zk.storage.get("o");
        out(o);
        o.a.push(99);                       // a fresh copy: changes nothing
        out((await zk.storage.get("o")).a.length);
        out(await zk.storage.get("missing") === undefined);
        out(await zk.storage.get("null") === null);
        out((await zk.storage.keys()).sort());
        out(await zk.storage.delete("n"));
        out(await zk.storage.delete("n"));
        out(await zk.storage.get("n") === undefined);
        try { await zk.storage.set("f", undefined); } catch (e) { out("undef: " + e.message); }
        try { await zk.storage.set("f", () => 1); } catch (e) { out("fn: " + e.name); }
    )");
    CHECK(contains(log, "OUT 5"));
    CHECK(contains(log, R"(OUT {"a":[1,2,{"b":null}],"s":"héllo 😀"})"));
    CHECK(contains(log, "OUT 3\n"));
    CHECK(contains(log, "OUT true"));
    CHECK(contains(log, R"(OUT ["n","null","o"])"));
    CHECK(contains(log, "OUT true"));
    CHECK(contains(log, "OUT false"));
    CHECK(contains(log, "undef: zk.storage: the value for \"f\" isn't JSON"));
    CHECK(contains(log, "fn: TypeError"));
    CHECK_FALSE(contains(log, "ERR"));
    CHECK(fs::exists(fp.path("a/storage.db")));
}

TEST_CASE("storage: values survive a restart of the host") {
    FakePlugins fp;
    runOnce(fp, "a", R"(await zk.storage.set("keep", { v: 1 });)");
    std::string log = runOnce(fp, "a", R"(out(await zk.storage.get("keep"));)");
    CHECK(contains(log, R"(OUT {"v":1})"));
}

TEST_CASE("storage: concurrent update() calls on one key apply in order (100 increments from interleaved handlers -> 100)") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        if (initial) { out(await zk.storage.update("n", (n) => n)); return; }
        await zk.storage.update("n", async (n = 0) => { await Promise.resolve(); await new Promise((r) => setTimeout(r, 0)); return n + 1; });
    )"));
    auto deps = fp.deps();
    deps.handlerBudgetMs = 4000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    for (int i = 0; i < 100; ++i) host.dispatch(kRun);
    host.dispatch(kSecond);
    REQUIRE(host.waitIdle(15000));
    std::string log = fp.log("a");
    CHECK(contains(log, "OUT 100"));
    CHECK_FALSE(contains(log, "ERR"));
}

TEST_CASE("storage: update returning undefined deletes") {
    FakePlugins fp;
    std::string log = runOnce(fp, "a", R"(
        await zk.storage.set("k", 1);
        out(await zk.storage.update("k", (v) => v === 1 ? undefined : v));
        out(await zk.storage.keys());
        out(await zk.storage.update("fresh", (v) => { out(v === undefined); return { made: true }; }));
        out(await zk.storage.update("k", async () => "later"));
    )");
    CHECK(contains(log, "OUT undefined"));
    CHECK(contains(log, "OUT []"));
    CHECK(contains(log, "OUT true"));
    CHECK(contains(log, R"(OUT {"made":true})"));
    CHECK(contains(log, "OUT later"));
}

TEST_CASE("storage: set, delete and update apply in the order asked") {
    FakePlugins fp;
    std::string log = runOnce(fp, "a", R"(
        const a = zk.storage.set("k", 1);
        const b = zk.storage.update("k", (v) => v + 10);
        const c = zk.storage.delete("k");
        const d = zk.storage.update("k", (v) => { out({ sawBeforeSet: v === undefined }); return "x"; });
        const e = zk.storage.set("k", "last");
        const f = zk.storage.update("k", (v) => v + "!");
        const rs = await Promise.all([a, b, c, d, e, f]);
        out(rs);
        out(await zk.storage.get("k"));
    )");
    CHECK(contains(log, R"(OUT {"sawBeforeSet":true})"));
    CHECK(contains(log, R"(OUT [null,11,true,"x",null,"last!"])"));
    CHECK(contains(log, "OUT last!"));
}

TEST_CASE("storage: an update whose fn throws or rejects fails alone; the queue moves on") {
    FakePlugins fp;
    std::string log = runOnce(fp, "a", R"(
        await zk.storage.set("k", 1);
        const a = zk.storage.update("k", () => { throw new Error("sync boom"); });
        const b = zk.storage.update("k", async () => { throw new Error("async boom"); });
        const c = zk.storage.update("k", (v) => v + 1);
        const rs = await Promise.allSettled([a, b, c]);
        out(rs.map((r) => r.status === "fulfilled" ? r.value : r.reason.message));
        out(await zk.storage.get("k"));
    )");
    CHECK(contains(log, R"(OUT ["sync boom","async boom",2])"));
    CHECK(contains(log, "OUT 2\n"));
}

TEST_CASE("storage: a write in the synchronous part of an update fn rejects at once naming the problem") {
    FakePlugins fp;
    std::string log = runOnce(fp, "a", R"(
        const seen = [];
        const r = await zk.storage.update("k", (v) => {
            for (const [name, call] of [["update", () => zk.storage.update("other", () => 1)],
                                        ["set", () => zk.storage.set("other", 1)],
                                        ["delete", () => zk.storage.delete("other")]]) {
                call().then(() => seen.push(name + " ok"), (e) => seen.push(name + ": " + e.message));
            }
            zk.storage.get("k").then((g) => seen.push("get " + g));   // reads are fine
            return 7;
        });
        await Promise.resolve(); await Promise.resolve();
        out(r);
        out(seen);
        out(await zk.storage.keys());
    )");
    CHECK(contains(log, "OUT 7"));
    CHECK(contains(log, "update: zk.storage.update can't be called inside another update's function"));
    CHECK(contains(log, "set: zk.storage.set can't be called inside another update's function"));
    CHECK(contains(log, "delete: zk.storage.delete can't be called inside another update's function"));
    CHECK(contains(log, "get undefined"));
    CHECK(contains(log, R"(OUT ["k"])"));
}

TEST_CASE("storage: a write after the fn's first await waits its turn (another handler's write isn't rejected)") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        if (!initial) {
            await zk.storage.update("slow", async () => {
                await new Promise((r) => setTimeout(r, 60));
                console.log("fn done");
                return 1;
            });
            console.log("update resolved");
        } else {
            await zk.storage.set("other", 2);          // another handler: waits, isn't rejected
            console.log("other set");
        }
    )"));
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    host.dispatch(kSecond);
    REQUIRE(host.waitIdle(5000));
    std::string log = fp.log("a");
    CHECK_FALSE(contains(log, "ERR"));
    REQUIRE(contains(log, "fn done"));
    REQUIRE(contains(log, "other set"));
    CHECK(where(log, "fn done") < where(log, "other set"));
}

TEST_CASE("storage: a write that waits on its own update ends the call with its LimitError; the plugin recovers") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        if (initial) { out(await zk.storage.get("kept")); out(await zk.storage.update("later", () => 1)); return; }
        await zk.storage.set("kept", "yes");
        await zk.storage.update("x", async () => { await Promise.resolve(); await zk.storage.set("y", 1); return 1; });
        console.log("not reached");
    )"));
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    host.dispatch(kSecond);
    REQUIRE(host.waitIdle(5000));
    std::string log = fp.log("a");
    CHECK_FALSE(contains(log, "not reached"));
    CHECK(contains(log, "Stopped"));
    CHECK(contains(log, "OUT yes"));
    CHECK(contains(log, "OUT 1"));
}

TEST_CASE("storage: two plugins don't see each other's keys") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(await zk.storage.set("mine", "a"); out(await zk.storage.keys()); out(await zk.storage.get("theirs") === undefined);)"));
    fp.add("b", pluginWith(R"(await zk.storage.set("theirs", "b"); out(await zk.storage.keys()); out(await zk.storage.get("mine") === undefined);)"));
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.setEnabled("b", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    CHECK(contains(fp.log("a"), R"(OUT ["mine"])"));
    CHECK(contains(fp.log("b"), R"(OUT ["theirs"])"));
    CHECK(contains(fp.log("a"), "OUT true"));
    CHECK(contains(fp.log("b"), "OUT true"));
    CHECK(fs::exists(fp.path("a/storage.db")));
    CHECK(fs::exists(fp.path("b/storage.db")));
}

TEST_CASE("storage: a value over 1 MB and a store over 16 MB are refused") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        try { await zk.storage.set("big", "x".repeat(1 << 20)); out("stored"); } catch (e) { out(e.message); }
        const chunk = "y".repeat(1000000);
        let n = 0;
        try { for (; n < 20; n++) await zk.storage.set("k" + n, chunk); } catch (e) { out(e.message); }
        out(n);
        await zk.storage.set("k0", "small");     // replacing frees its old size
        out(await zk.storage.get("k0"));
    )"));
    auto deps = fp.deps();
    deps.handlerBudgetMs = 4000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(10000));
    std::string log = fp.log("a");
    CHECK(contains(log, "one value may be at most 1 MB"));
    CHECK(contains(log, "past 16 MB"));
    CHECK(contains(log, "OUT 16\n"));
    CHECK(contains(log, "OUT small"));
}

TEST_CASE("files: write/read/list/remove inside data/") {
    FakePlugins fp;
    std::string log = runOnce(fp, "a", R"(
        await zk.files.write("highlights/Gaussian random fields.md", "# héllo \u{1F600}\n");
        await zk.files.write("top.txt", "t");
        await zk.files.write("./highlights//deep/x.txt", "x");
        out({ text: await zk.files.read("highlights/Gaussian random fields.md") });
        out(await zk.files.list());
        out(await zk.files.list("highlights"));
        out(await zk.files.list("nope"));
        await zk.files.write("top.txt", "replaced");
        out(await zk.files.read("top.txt"));
        out(await zk.files.remove("top.txt"));
        out(await zk.files.remove("top.txt"));
        out(await zk.files.remove("highlights"));
        out(await zk.files.list());
        try { await zk.files.read("gone.txt"); } catch (e) { out("read: " + e.message); }
        try { await zk.files.write("", "x"); } catch (e) { out("empty: " + e.message); }
        try { await zk.files.write("big.txt", "z".repeat(17 << 20)); } catch (e) { out("big: " + e.message); }
    )");
    CHECK(contains(log, R"(OUT {"text":"# héllo 😀\n"})"));
    CHECK(contains(log, R"(OUT ["highlights/","top.txt"])"));
    CHECK(contains(log, R"(OUT ["Gaussian random fields.md","deep/"])"));
    CHECK(contains(log, "OUT []"));
    CHECK(contains(log, "OUT replaced"));
    CHECK(contains(log, "read: zk.files: \"gone.txt\" doesn't exist"));
    CHECK(contains(log, "empty: zk.files: a path is needed"));
    CHECK(contains(log, "at most 16 MB"));
    CHECK_FALSE(fp.exists("a/data/highlights"));
    CHECK_FALSE(fp.exists("a/data/top.txt"));
    CHECK(fp.exists("a/data"));
    std::string text = fp.read("a/data/x");  // nothing odd left behind
    CHECK(text.empty());
}

TEST_CASE("files: ../, absolute paths and a symlink out of data/ are refused") {
    FakePlugins fp;
    fp.add("a", pluginWith("out('ready');"));
    fp.write("outside/secret.txt", "TOP SECRET");
    fp.write("a/data/inside.txt", "ok");
    fs::create_symlink(fp.path("outside"), fp.path("a/data/link"));
    fs::create_symlink(fp.path("outside/secret.txt"), fp.path("a/data/filelink.txt"));
    fp.add("a", pluginWith(R"(
        const tries = {
          "dotdot read": () => zk.files.read("../outside/secret.txt"),
          "dotdot inner": () => zk.files.read("sub/../../x"),
          "dotdot write": () => zk.files.write("../escaped.txt", "x"),
          "abs read": () => zk.files.read("/etc/passwd"),
          "abs list": () => zk.files.list("/"),
          "abs remove": () => zk.files.remove("/tmp"),
          "link read": () => zk.files.read("link/secret.txt"),
          "link write": () => zk.files.write("link/new.txt", "x"),
          "link list": () => zk.files.list("link"),
          "link remove": () => zk.files.remove("link/secret.txt"),
          "filelink read": () => zk.files.read("filelink.txt"),
          "filelink write": () => zk.files.write("filelink.txt", "overwritten"),
        };
        for (const [name, call] of Object.entries(tries)) {
          try { await call(); out(name + " ALLOWED"); } catch (e) { out(name + " -> " + e.message); }
        }
        out(await zk.files.read("inside.txt"));
    )"));
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    std::string log = fp.log("a");
    CHECK_FALSE(contains(log, "ALLOWED"));
    CHECK(contains(log, "dotdot read -> zk.files: \"../outside/secret.txt\" is outside the plugin's data folder"));
    CHECK(contains(log, "abs read -> zk.files: \"/etc/passwd\" is outside"));
    CHECK(contains(log, "link read -> zk.files: \"link/secret.txt\" is outside"));
    CHECK(contains(log, "filelink write -> zk.files: \"filelink.txt\" is outside"));
    CHECK(contains(log, "OUT ok"));
    CHECK(fp.read("outside/secret.txt") == "TOP SECRET");
    CHECK_FALSE(fp.exists("outside/new.txt"));
    CHECK_FALSE(fp.exists("a/escaped.txt"));
}

TEST_CASE("files: resetData empties data/ and storage") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        if (!initial) {
            await zk.storage.set("k", 1);
            await zk.files.write("sub/f.txt", "x");
            out("filled");
        } else {
            out({ k: await zk.storage.get("k") === undefined, keys: await zk.storage.keys(), files: await zk.files.list() });
            await zk.storage.set("after", 2);                 // storage works again after a reset
            out(await zk.storage.keys());
        }
    )"));
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    REQUIRE(fp.exists("a/data/sub/f.txt"));
    host.resetData("a");
    REQUIRE(host.waitIdle(5000));
    CHECK_FALSE(fp.exists("a/data/sub"));
    host.dispatch(kSecond);
    REQUIRE(host.waitIdle(5000));
    std::string log = fp.log("a");
    CHECK(contains(log, R"(OUT {"k":true,"keys":[],"files":[]})"));
    CHECK(contains(log, R"(OUT ["after"])"));
    CHECK_FALSE(contains(log, "ERR"));
}

TEST_CASE("files: resetData works for a plugin that isn't loaded, and drops queued writes of one that is") {
    {
        FakePlugins fp;
        fp.add("a", pluginWith("out(1);"));
        fp.write("a/data/x/y.txt", "y");
        fp.write("a/storage.db", "junk");
        fp.write("a/storage.db-wal", "junk");
        PluginHost host(fp.deps());
        host.start();
        host.resetData("a");
        host.resetData("../escape");  // not a plugin: ignored
        REQUIRE(host.waitIdle(5000));
        CHECK_FALSE(fp.exists("a/data/x"));
        CHECK_FALSE(fp.exists("a/storage.db"));
        CHECK_FALSE(fp.exists("a/storage.db-wal"));
        CHECK(fp.exists("a/plugin.json"));
    }
    {
        FakePlugins fp;
        fp.add("a", pluginWith(R"(
            if (!initial) {
                const slow = zk.storage.update("k", async () => { await new Promise((r) => setTimeout(r, 100)); return "stale"; });
                const queued = zk.storage.set("q", "queued");
                const rs = await Promise.allSettled([slow, queued]);
                out(rs.map((r) => r.status === "fulfilled" ? "ok" : r.reason.message));
            } else {
                out(await zk.storage.keys());
            }
        )"));
        PluginHost host(fp.deps());
        host.setEnabled("a", true);
        host.start();
        host.dispatch(kRun);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        host.resetData("a");
        REQUIRE(host.waitIdle(5000));
        host.dispatch(kSecond);
        REQUIRE(host.waitIdle(5000));
        std::string log = fp.log("a");
        CHECK(contains(log, "zk.storage was reset"));
        CHECK(contains(log, "OUT []"));
    }
}

TEST_CASE("storage: set stores the value as it was when set() was called, and refuses a bad one at once") {
    FakePlugins fp;
    std::string log = runOnce(fp, "a", R"(
        const arr = [1];
        const p = zk.storage.set("k", arr);
        arr.push(2);
        let toJsonCalls = 0;
        const q = zk.storage.set("t", { toJSON() { toJsonCalls++; return "called"; } });
        out("toJSON calls right after set(): " + toJsonCalls);
        // Blocked behind a slow update, a bad value still rejects at once.
        const slow = zk.storage.update("slow", async () => { await new Promise((r) => setTimeout(r, 80)); return 1; });
        let order = [];
        const bad1 = zk.storage.set("u", undefined).catch((e) => order.push("undefined: " + e.name));
        const bad2 = zk.storage.set("f", () => 1).catch((e) => order.push("function: " + e.name));
        slow.then(() => order.push("slow done"));
        await Promise.all([p, q, bad1, bad2, slow]);
        out(order);
        out(await zk.storage.get("k"));
        out(await zk.storage.get("t"));
        const r = await zk.storage.update("k", (v) => { const w = [...v, 3]; setTimeout(() => w.push("late"), 0); return w; });
        out(r);
    )");
    CHECK(contains(log, "OUT toJSON calls right after set(): 1"));
    CHECK(contains(log, R"(OUT ["undefined: TypeError","function: TypeError","slow done"])"));
    CHECK(contains(log, "OUT [1]\n"));
    CHECK(contains(log, "OUT called"));
    CHECK(contains(log, "OUT [1,3]"));
    CHECK_FALSE(contains(log, "ERR"));
}

TEST_CASE("storage: the store's size total stays right across overwrite, delete and reset") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        const chunk = "y".repeat(1000000);
        const tryset = async (k, v) => { try { await zk.storage.set(k, v); return "ok"; } catch (e) { return "refused"; } };
        const res = [];
        if (!initial) {
            for (let i = 0; i < 16; i++) res.push(await tryset("k" + i, chunk));
            res.push(await tryset("k16", chunk));        // over the cap
            await zk.storage.set("k0", "small");          // overwrite frees ~1 MB
            res.push(await tryset("k16", chunk));         // fits now
            res.push(await tryset("k17", chunk));         // full again
            await zk.storage.delete("k1"); await zk.storage.delete("k2");
            res.push(await tryset("k17", chunk));
            res.push(await tryset("k18", chunk));
            res.push(await tryset("k19", chunk));         // full again
            out(res.join(","));
        } else {
            for (let i = 0; i < 16; i++) res.push(await tryset("n" + i, chunk));
            res.push(await tryset("n16", chunk));
            out(res.join(","));
        }
    )"));
    auto deps = fp.deps();
    deps.handlerBudgetMs = 6000;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(15000));
    host.resetData("a");
    host.dispatch(kSecond);
    REQUIRE(host.waitIdle(15000));
    std::string log = fp.log("a");
    const std::string sixteenOk = "ok,ok,ok,ok,ok,ok,ok,ok,ok,ok,ok,ok,ok,ok,ok,ok";
    CHECK(contains(log, "OUT " + sixteenOk + ",refused,ok,refused,ok,ok,refused\n"));
    CHECK(contains(log, "OUT " + sixteenOk + ",refused\n"));
}

TEST_CASE("files: resetData on a data/ that is a symlink removes the link, not what it points to") {
    FakePlugins fp;
    fp.add("a", pluginWith("out(1);"));
    fp.write("outside/precious.txt", "keep me");
    fs::create_symlink(fp.path("outside"), fp.path("a/data"));
    PluginHost host(fp.deps());
    host.start();
    host.resetData("a");
    REQUIRE(host.waitIdle(5000));
    CHECK(fp.read("outside/precious.txt") == "keep me");
    CHECK_FALSE(fs::is_symlink(fp.path("a/data")));
}

TEST_CASE("storage: an update waiting in the queue holds the call that asked, and its fn runs once") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        if (!initial) {
            zk.storage.update("slow", async () => { await new Promise((r) => setTimeout(r, 100)); return 1; });
            zk.storage.update("n", (n = 0) => { console.log("queued fn ran"); return n + 1; });   // not awaited
        } else {
            out(await zk.storage.update("n", (n) => n));
        }
    )"));
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    host.dispatch(kSecond);
    REQUIRE(host.waitIdle(5000));
    std::string log = fp.log("a");
    CHECK(contains(log, "OUT 1"));
    CHECK(where(log, "queued fn ran") == log.rfind("queued fn ran"));
    CHECK_FALSE(contains(log, "ERR"));
}

namespace {
// zk.test.later(ms): host work that settles after ms (startWork, like a real binding).
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
void installLater(plugins::PluginCtx& p) {
    JSContext* c = p.cx.raw();
    JSValue t = JS_NewObject(c);
    plugins::defineFunction<testLater>(c, t, "later", 1);
    JS_SetPropertyStr(c, p.zk.raw(), "test", t);
}
const PluginEvent evA{"syncFinished", {{"origin", "A"}}, false};
const PluginEvent evB{"syncFinished", {{"origin", "B"}}, false};
const PluginEvent evC{"syncFinished", {{"origin", "C"}}, false};
}  // namespace

TEST_CASE("storage: the update queued after a stopped asker's runs in a call of its own, not the stopped one's") {
    FakePlugins fp;
    fp.add("a", "zk.on('syncFinished', async ({ origin }) => { try {\n"
                "  if (origin === 'A') {\n"
                "    await zk.storage.update('x', async () => { await new Promise((r) => setTimeout(r, 2000)); return 1; });\n"
                "  } else {\n"
                "    const r = await zk.storage.update('y', async () => { const w = await zk.test.later(10); await zk.storage.get('y'); return 'b ' + w; });\n"
                "    console.log('B result ' + r);\n"
                "  }\n"
                "} catch (e) { console.log('FAILED ' + origin + ': ' + e.name + ' ' + e.message); } });\n");
    auto deps = fp.deps();
    deps.extraBindings.push_back(installLater);
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(evA);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    host.dispatch(evB);  // queued behind A's update; alive for 300 ms after its own start
    REQUIRE(host.waitIdle(5000));
    std::string log = fp.log("a");
    CHECK(contains(log, "FAILED A: LimitError"));
    CHECK(contains(log, "B result b done"));
    CHECK_FALSE(contains(log, "FAILED B"));
}

TEST_CASE("storage: an asker stopped while its update waits in the queue fails it without running fn; the queue moves on") {
    FakePlugins fp;
    fp.add("a", "zk.on('syncFinished', async ({ origin }) => { try {\n"
                "  if (origin === 'A') {\n"
                "    await zk.storage.update('a', () => { console.log('A fn ran'); return 1; });\n"
                "  } else {\n"
                "    await zk.storage.update('c', () => { console.log('C fn ran'); return 1; });\n"
                "  }\n"
                "} catch (e) { console.log('FAILED ' + origin + ': ' + e.name + ' ' + e.limit); } });\n"
                "zk.background(async () => { await zk.storage.update('s', async () => { await new Promise((r) => setTimeout(r, 700)); console.log('S fn done'); return 1; }); });\n",
           {"background"});
    auto deps = fp.deps();
    deps.handlerBudgetMs = 500;
    deps.backgroundBudgetMs = 1500;
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.runBackground();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    host.dispatch(evA);  // asks at ~100 ms, stopped at ~600 ms while still waiting behind S
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    host.dispatch(evC);  // asks at ~500 ms behind A's update; its own deadline is ~1000 ms
    REQUIRE(host.waitIdle(8000));
    std::string log = fp.log("a");
    CHECK(contains(log, "S fn done"));
    CHECK(contains(log, "FAILED A: LimitError time"));
    CHECK_FALSE(contains(log, "A fn ran"));
    CHECK(contains(log, "C fn ran"));
    CHECK_FALSE(contains(log, "FAILED C"));
}

TEST_CASE("files: a plugin's data/ holds at most 64 MB in all; overwrite and remove free room") {
    FakePlugins fp;
    // 60 MB already there (sparse: cheap to make, counted by size).
    fp.write("a/data/old/big.bin", "");
    fs::resize_file(fp.path("a/data/old/big.bin"), 60u << 20);
    fp.add("a", pluginWith(R"(
        const mb = (n) => "z".repeat(n << 20);
        await zk.files.write("one.txt", mb(3));
        out("3 MB fits");
        try { await zk.files.write("two.txt", mb(2)); out("wrote two"); } catch (e) { out("two: " + e.message); }
        await zk.files.write("one.txt", mb(1));
        out("overwrite to 1 MB fits");
        await zk.files.write("two.txt", mb(2));
        out("now two fits");
        out(await zk.files.remove("old"));
        await zk.files.write("three.txt", mb(15));
        out("after the removal 15 MB fits");
    )"));
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(10000));
    const std::string log = fp.log("a");
    CHECK(contains(log, "OUT 3 MB fits"));
    CHECK(contains(log, "OUT two: zk.files: writing \"two.txt\" would take this plugin's data folder past 64 MB"));
    CHECK_FALSE(contains(log, "wrote two"));
    CHECK(contains(log, "OUT overwrite to 1 MB fits"));
    CHECK(contains(log, "OUT now two fits"));
    CHECK(contains(log, "OUT after the removal 15 MB fits"));
    CHECK_FALSE(fs::exists(fp.path("a/data/two.txt.zk-tmp")));
}

TEST_CASE("files and storage: writes are refused when the Kindle has less than 50 MB free") {
    FakePlugins fp;
    fp.add("a", pluginWith(R"(
        try { await zk.files.write("x.txt", "x"); out("file written"); } catch (e) { out("file: " + e.message); }
        try { await zk.storage.set("k", 1); out("stored"); } catch (e) { out("storage: " + e.message); }
        out(await zk.storage.get("k") === undefined);
    )"));
    auto deps = fp.deps();
    deps.freeSpace = [](const std::string&) -> uint64_t { return 40u << 20; };
    PluginHost host(deps);
    host.setEnabled("a", true);
    host.start();
    host.dispatch(kRun);
    REQUIRE(host.waitIdle(5000));
    const std::string log = fp.log("a");
    CHECK(contains(log, "OUT file: zk.files: \"x.txt\" wasn't written: the Kindle has less than 50 MB free"));
    CHECK(contains(log, "OUT storage: zk.storage: \"k\" wasn't stored: the Kindle has less than 50 MB free"));
    CHECK(contains(log, "OUT true"));
    CHECK_FALSE(fs::exists(fp.path("a/data/x.txt")));
}
