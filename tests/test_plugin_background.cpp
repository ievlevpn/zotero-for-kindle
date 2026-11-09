// Task 15: background tasks (after a successful sync, at most hourly, inside a keep-awake hold)
// and badges.

#include <doctest/doctest.h>

#include <atomic>

#include "app/plugin_background.h"
#include "fake_lipc.h"
#include "fake_plugins.h"
#include "fn_http.h"
#include "platform/power.h"
#include "plugins/host.h"
#include "plugins/state.h"

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kHour = 3600 * 1000;

size_t count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

int failuresNow(const FakePlugins& fp, const std::string& id) {
    PluginState s(fp.path(".state.json"));
    return s.failures(id);
}

SyncEvent syncFinished(bool failed) {
    SyncEvent e;
    e.kind = SyncEvent::Kind::SyncFinished;
    if (failed) e.error = UserError{ErrorKind::NoWifi, "No connection", ""};
    return e;
}

// All events the host has queued so far.
std::vector<PluginHostEvent> drain(PluginHost& host) {
    std::vector<PluginHostEvent> out;
    PluginHostEvent ev;
    while (host.poll(ev)) out.push_back(ev);
    return out;
}

size_t ofKind(const std::vector<PluginHostEvent>& evs, PluginHostEvent::Kind k) {
    size_t n = 0;
    for (const auto& e : evs) n += e.kind == k;
    return n;
}

struct Clocked {
    std::atomic<int64_t> now{1'000'000'000'000};
    PluginHostDeps deps(const FakePlugins& fp) {
        PluginHostDeps d = fp.deps();
        d.clock = [this] { return now.load(); };
        return d;
    }
};

}  // namespace

TEST_CASE("background: runs after a successful sync, not after a failed one") {
    FakeLipc lipc;
    KeepAwake awake(lipc);
    int runs = 0, cancels = 0;
    uint64_t last = 0;
    BackgroundRound round(awake, [&](uint64_t r) { ++runs; last = r; }, [&] { ++cancels; });

    round.onSyncEvent(syncFinished(true), true, 0);
    CHECK(runs == 0);
    CHECK_FALSE(round.holding());
    CHECK(lipc.sets.empty());

    SyncEvent other;
    other.kind = SyncEvent::Kind::PushFinished;
    round.onSyncEvent(other, true, 0);
    CHECK(runs == 0);

    round.onSyncEvent(syncFinished(false), false, 0);  // the Kindle went to the background: never wakes it
    CHECK(runs == 0);

    round.onSyncEvent(syncFinished(false), true, 0);
    CHECK(runs == 1);
    CHECK(round.holding());
    CHECK(lipc.sets == std::vector<std::string>{"com.lab126.powerd preventScreenSaver 1"});

    // The hold ends when the host reports the round done ...
    round.done(last);
    CHECK_FALSE(round.holding());
    CHECK(lipc.sets.back() == "com.lab126.powerd preventScreenSaver 0");
    round.done(last);  // a stray report changes nothing
    CHECK(lipc.sets.size() == 2);

    // ... or at the cap (35 s) ...
    round.onSyncEvent(syncFinished(false), true, 100'000);
    CHECK(runs == 2);
    round.tick(100'000 + BackgroundRound::kCapMs - 1);
    CHECK(round.holding());
    round.tick(100'000 + BackgroundRound::kCapMs);
    CHECK_FALSE(round.holding());
    CHECK(lipc.sets.back() == "com.lab126.powerd preventScreenSaver 0");
    CHECK(cancels == 0);  // the host's own 30 s budget ends the task; the cap only lets the Kindle sleep

    // ... or when the Kindle goes to sleep, which also cuts the task short.
    round.onSyncEvent(syncFinished(false), true, 200'000);
    round.onSleep();
    CHECK_FALSE(round.holding());
    CHECK(cancels == 1);

    // Tasks still running past the cap are cut short by sleep too.
    round.onSyncEvent(syncFinished(false), true, 300'000);
    round.tick(300'000 + BackgroundRound::kCapMs);
    CHECK_FALSE(round.holding());
    round.onSleep();
    CHECK(cancels == 2);
}

TEST_CASE("background: no round while offline or when no task is due") {
    FakeLipc lipc;
    KeepAwake awake(lipc);
    int runs = 0;
    bool ready = false;
    BackgroundRound round(awake, [&](uint64_t) { ++runs; }, [] {}, [&] { return ready; });
    round.onSyncEvent(syncFinished(false), true, 0);
    CHECK(runs == 0);
    CHECK_FALSE(round.holding());
    CHECK(lipc.sets.empty());
    ready = true;
    round.onSyncEvent(syncFinished(false), true, 0);
    CHECK(runs == 1);
    CHECK(round.holding());
}

TEST_CASE("background: nothing due: no hold and no plugin loaded") {
    FakePlugins fp;
    fp.add("nopermission", R"(console.log("loaded");)");  // enabled, but no background permission
    fp.add("off", R"(zk.background(() => {});)", {"background"});  // has it, but turned off
    Clocked clk;
    PluginHost host(clk.deps(fp));
    host.setEnabled("nopermission", true);
    host.start();
    FakeLipc lipc;
    KeepAwake awake(lipc);
    BackgroundRound round(awake, [&](uint64_t r) { host.runBackground(r); }, [] {}, [&] { return host.backgroundDue(); });
    CHECK_FALSE(host.backgroundDue());
    round.onSyncEvent(syncFinished(false), true, 0);
    CHECK_FALSE(round.holding());
    CHECK(lipc.sets.empty());
    REQUIRE(host.waitIdle(3000));
    CHECK_FALSE(contains(fp.log("nopermission"), "loaded"));
    for (const auto& st : host.status()) CHECK_FALSE(st.loaded);

    // A plugin with the permission that has never run counts as possibly due, once; after its run
    // it is due again an hour later. A loaded plugin that registered no task is never due.
    fp.add("b", R"(zk.background(() => {});)", {"background"});
    fp.add("c", R"(console.log("no task");)", {"background"});
    host.reload();
    host.setEnabled("b", true);
    host.setEnabled("c", true);
    REQUIRE(host.waitIdle(3000));
    CHECK(host.backgroundDue());
    host.runBackground(1);
    REQUIRE(host.waitIdle(3000));
    CHECK_FALSE(host.backgroundDue());
    clk.now += kHour;
    CHECK(host.backgroundDue());
}

TEST_CASE("background: a late report of an earlier round doesn't end the next one") {
    FakeLipc lipc;
    KeepAwake awake(lipc);
    std::vector<uint64_t> rounds;
    BackgroundRound round(awake, [&](uint64_t r) { rounds.push_back(r); }, [] {});
    round.onSyncEvent(syncFinished(false), true, 0);
    REQUIRE(rounds.size() == 1);
    round.done(rounds[0]);
    CHECK_FALSE(round.holding());
    round.onSyncEvent(syncFinished(false), true, 1000);
    REQUIRE(rounds.size() == 2);
    CHECK(rounds[1] != rounds[0]);
    round.done(rounds[0]);  // round 1's report, arriving late
    CHECK(round.holding());
    round.done(rounds[1]);
    CHECK_FALSE(round.holding());
}

TEST_CASE("background: the host's Done carries the round it answers") {
    FakePlugins fp;
    fp.add("a", R"(zk.background(() => {});)", {"background"});
    Clocked clk;
    PluginHost host(clk.deps(fp));
    host.setEnabled("a", true);
    host.start();
    host.runBackground(7);
    host.runBackground(8);  // nothing due: Done at once
    REQUIRE(host.waitIdle(3000));
    std::vector<uint64_t> ids;
    for (const auto& e : drain(host))
        if (e.kind == PluginHostEvent::Kind::BackgroundDone) ids.push_back(e.requestId);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 7);
    CHECK(ids[1] == 8);
}

// A plugin whose task fetches; `answer` is what the network gives.
struct FetchRig {
    FakePlugins fp;
    Clocked clk;
    std::atomic<int> curlCode{7};  // couldn't connect
    std::atomic<long> status{200};
    PluginHostDeps deps(const std::string& body) {
        fp.add("a", "zk.background(async () => {\n" + body + "\n});", {"background", "net:example.org"});
        PluginHostDeps d = clk.deps(fp);
        d.makeHttp = [this] {
            return std::make_unique<FnHttp>([this](const HttpRequest&) {
                HttpResponse r;
                r.curlCode = curlCode;
                r.status = status;
                return r;
            });
        };
        return d;
    }
};

TEST_CASE("background: a task that dies of a lost connection is not counted and runs again") {
    FetchRig rig;
    PluginHost host(rig.deps(R"(console.log("try"); await zk.fetch("https://example.org/x"); console.log("fetched");)"));
    host.setEnabled("a", true);
    host.start();
    host.runBackground(1);
    REQUIRE(host.waitIdle(5000));
    CHECK(count(rig.fp.log("a"), "try") == 1);
    CHECK(failuresNow(rig.fp, "a") == 0);
    {
        PluginState s(rig.fp.path(".state.json"));
        CHECK(s.lastBackgroundMs("a") == 0);
    }
    CHECK(ofKind(drain(host), PluginHostEvent::Kind::BackgroundDone) == 1);
    // A few flaky hours in a row don't turn it off.
    for (int i = 0; i < 2; ++i) {
        host.runBackground(2 + i);
        REQUIRE(host.waitIdle(5000));
    }
    CHECK(count(rig.fp.log("a"), "try") == 3);
    CHECK(failuresNow(rig.fp, "a") == 0);

    // The connection is back: it runs, and now the hour is used.
    rig.curlCode = 0;
    host.runBackground(10);
    REQUIRE(host.waitIdle(5000));
    CHECK(contains(rig.fp.log("a"), "fetched"));
    PluginState s(rig.fp.path(".state.json"));
    CHECK(s.lastBackgroundMs("a") == rig.clk.now.load());
}

TEST_CASE("background: only the real network error is forgiven; a forged or new one counts") {
    const char* bodies[] = {
        R"(await zk.fetch("https://example.org/x");)",                                                  // the real one escapes
        R"(try { await zk.fetch("https://example.org/x"); } catch (e) { throw e; })",                    // rethrown: the same object
        R"(throw Object.assign(new Error("x"), { network: true });)",                                    // forged
        R"(throw { network: true };)",                                                                   // a plain object
        R"(try { await zk.fetch("https://example.org/x"); } catch (e) { throw new Error("again: " + e.message); })",  // a new one
        R"(try { await zk.fetch("https://example.org/x"); } catch (e) { throw Object.assign(new Error(e.message), e); })",  // a copy
    };
    const bool forgiven[] = {true, true, false, false, false, false};
    for (int i = 0; i < 6; ++i) {
        CAPTURE(i);
        FetchRig rig;
        PluginHost host(rig.deps(bodies[i]));
        host.setEnabled("a", true);
        host.start();
        host.runBackground(1);
        REQUIRE(host.waitIdle(5000));
        PluginState s(rig.fp.path(".state.json"));
        CHECK(s.failures("a") == (forgiven[i] ? 0 : 1));
        CHECK(s.lastBackgroundMs("a") == (forgiven[i] ? 0 : rig.clk.now.load()));
    }
}

TEST_CASE("background: the 4th uncounted round in a row counts") {
    FetchRig rig;
    PluginHost host(rig.deps(R"(await zk.fetch("https://example.org/x");)"));
    host.setEnabled("a", true);
    host.start();
    for (int i = 0; i < 3; ++i) {
        host.runBackground(1 + i);
        REQUIRE(host.waitIdle(5000));
        CHECK(failuresNow(rig.fp, "a") == 0);
    }
    host.runBackground(4);
    REQUIRE(host.waitIdle(5000));
    CHECK(failuresNow(rig.fp, "a") == 1);
    PluginState s(rig.fp.path(".state.json"));
    CHECK(s.lastBackgroundMs("a") == rig.clk.now.load());
}

TEST_CASE("background: an HTTP 500 the plugin throws still counts") {
    FetchRig rig;
    rig.curlCode = 0;
    rig.status = 500;
    PluginHost host(rig.deps(R"(const r = await zk.fetch("https://example.org/x"); if (r.status >= 500) throw new Error("HTTP " + r.status);)"));
    host.setEnabled("a", true);
    host.start();
    host.runBackground(1);
    REQUIRE(host.waitIdle(5000));
    CHECK(failuresNow(rig.fp, "a") == 1);
    PluginState s(rig.fp.path(".state.json"));
    CHECK(s.lastBackgroundMs("a") == rig.clk.now.load());
}

TEST_CASE("background: a task awaiting something slow is stopped at its budget and counted") {
    FakePlugins fp;
    fp.add("a", R"(zk.background(async () => { await new Promise((r) => setTimeout(r, 60000)); });)", {"background"});
    Clocked clk;
    PluginHost host(clk.deps(fp));
    host.setEnabled("a", true);
    host.start();
    host.runBackground(1);
    REQUIRE(host.waitIdle(5000));
    CHECK(failuresNow(fp, "a") == 1);
    PluginState s(fp.path(".state.json"));
    CHECK(s.lastBackgroundMs("a") == clk.now.load());
    auto all = host.status();
    REQUIRE(statusOf(all, "a"));
    CHECK(contains(statusOf(all, "a")->errors.back(), "took longer than"));
}

TEST_CASE("background: at most once an hour per plugin") {
    FakePlugins fp;
    fp.add("a", R"(zk.background(() => console.log("ran a"));)", {"background"});
    fp.add("b", R"(zk.background(() => console.log("ran b"));)", {"background"});
    Clocked clk;
    PluginHost host(clk.deps(fp));
    host.setEnabled("a", true);
    host.start();
    host.runBackground();
    REQUIRE(host.waitIdle(3000));
    CHECK(count(fp.log("a"), "ran a") == 1);
    {
        PluginState s(fp.path(".state.json"));
        CHECK(s.lastBackgroundMs("a") == clk.now.load());
    }

    clk.now += kHour - 1;
    host.runBackground();
    REQUIRE(host.waitIdle(3000));
    CHECK(count(fp.log("a"), "ran a") == 1);

    host.setEnabled("b", true);  // its own hour: b has never run
    host.runBackground();
    REQUIRE(host.waitIdle(3000));
    CHECK(count(fp.log("a"), "ran a") == 1);
    CHECK(count(fp.log("b"), "ran b") == 1);

    clk.now += 1;
    host.runBackground();
    REQUIRE(host.waitIdle(3000));
    CHECK(count(fp.log("a"), "ran a") == 2);
    CHECK(count(fp.log("b"), "ran b") == 1);

    // One "round done" per runBackground(), also when nothing was due.
    const auto evs = drain(host);
    CHECK(ofKind(evs, PluginHostEvent::Kind::BackgroundDone) == 4);
}

TEST_CASE("background: needs the background permission") {
    FakePlugins fp;
    fp.add("a", R"(
        try { zk.background(() => console.log("ran")); console.log("registered"); }
        catch (e) { console.log(e instanceof zk.PermissionError, e.permission); }
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    host.runBackground();
    REQUIRE(host.waitIdle(3000));
    CHECK(contains(fp.log("a"), "true background"));
    CHECK_FALSE(contains(fp.log("a"), "registered"));
    CHECK_FALSE(contains(fp.log("a"), "ran\n"));
}

TEST_CASE("background: over 30 s it is stopped and counted") {
    FakePlugins fp;  // the budget is 600 ms in tests
    fp.add("a", R"(zk.background(() => { for (;;) {} });)", {"background"});
    Clocked clk;
    PluginHost host(clk.deps(fp));
    host.setEnabled("a", true);
    host.start();
    host.runBackground();
    REQUIRE(host.waitIdle(5000));
    auto all = host.status();
    auto* st = statusOf(all, "a");
    REQUIRE(st);
    REQUIRE(st->errors.size() == 1);
    CHECK(contains(st->errors[0], "took longer than"));
    CHECK(st->enabled);
    {
        PluginState s(fp.path(".state.json"));
        CHECK(s.failures("a") == 1);
        CHECK(s.lastBackgroundMs("a") == clk.now.load());  // retried in an hour, not at the next sync
    }
    auto evs = drain(host);
    CHECK(ofKind(evs, PluginHostEvent::Kind::BackgroundDone) == 1);

    // Three in a row turn the plugin off.
    for (int i = 0; i < 2; ++i) {
        clk.now += kHour;
        host.runBackground();
        REQUIRE(host.waitIdle(5000));
    }
    all = host.status();
    CHECK_FALSE(statusOf(all, "a")->enabled);
    evs = drain(host);
    CHECK(ofKind(evs, PluginHostEvent::Kind::Disabled) == 1);
}

TEST_CASE("background: cut short by stop() is not counted") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.background(async () => {
          console.log("started");
          await new Promise((r) => setTimeout(r, 60000));
          console.log("finished");
        });
    )", {"background"});
    Clocked clk;
    {
        PluginHost host(clk.deps(fp));
        host.setEnabled("a", true);
        host.start();
        host.runBackground();
        REQUIRE(eventually([&] { return contains(fp.log("a"), "started"); }));
        host.stop();
    }
    PluginState s(fp.path(".state.json"));
    CHECK(s.failures("a") == 0);
    CHECK(s.lastBackgroundMs("a") == 0);  // retried at the next sync
}

TEST_CASE("background: cut short by sleep is not counted and runs again next time") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.background(async () => {
          console.log("started");
          await new Promise((r) => setTimeout(r, 60000));
          console.log("finished");
        });
    )", {"background"});
    Clocked clk;
    PluginHost host(clk.deps(fp));
    host.setEnabled("a", true);
    host.start();
    host.runBackground();
    REQUIRE(eventually([&] { return contains(fp.log("a"), "started"); }));
    host.cancelBackground();
    REQUIRE(host.waitIdle(3000));
    {
        PluginState s(fp.path(".state.json"));
        CHECK(s.failures("a") == 0);
        CHECK(s.lastBackgroundMs("a") == 0);
    }
    CHECK(statusOf(host.status(), "a") != nullptr);
    CHECK(failuresNow(fp, "a") == 0);
    CHECK(ofKind(drain(host), PluginHostEvent::Kind::BackgroundDone) == 1);
    host.runBackground();
    REQUIRE(eventually([&] { return count(fp.log("a"), "started") == 2; }));
    host.cancelBackground();
    REQUIRE(host.waitIdle(3000));
    CHECK_FALSE(contains(fp.log("a"), "finished"));
}

TEST_CASE("background: setBadge posts Badge and persists") {
    FakePlugins fp;
    fp.add("a", R"(
        zk.on("syncFinished", ({ text }) => {
          zk.setBadge(text === "long" ? "x".repeat(100) : text);
        });
    )");
    PluginHost host(fp.deps());
    host.setEnabled("a", true);
    host.start();
    auto send = [&](nlohmann::ordered_json text) {
        host.dispatch({"syncFinished", {{"text", text}}, false});
        REQUIRE(host.waitIdle(3000));
        return drain(host);
    };

    auto evs = send("  3 new\n items ");  // one line
    REQUIRE(ofKind(evs, PluginHostEvent::Kind::Badge) == 1);
    CHECK(evs.back().pluginId == "a");
    CHECK(evs.back().text == "3 new items");
    {
        auto all = host.status();
        CHECK(statusOf(all, "a")->badge == "3 new items");
    }
    {
        PluginState s(fp.path(".state.json"));  // across restarts
        CHECK(s.badge("a") == "3 new items");
    }

    evs = send("long");  // long text is cut
    REQUIRE(ofKind(evs, PluginHostEvent::Kind::Badge) == 1);
    {
        size_t chars = 0;
        for (unsigned char ch : evs.back().text) chars += (ch & 0xC0) != 0x80;
        CHECK(chars == 40);
        CHECK(evs.back().text.substr(evs.back().text.size() - 3) == "\xE2\x80\xA6");
    }

    evs = send(nullptr);  // null clears
    REQUIRE(ofKind(evs, PluginHostEvent::Kind::Badge) == 1);
    CHECK(evs.back().text.empty());
    {
        auto all = host.status();
        CHECK(statusOf(all, "a")->badge.empty());
    }

    CHECK(ofKind(send(""), PluginHostEvent::Kind::Badge) == 0);  // clearing a cleared badge says nothing
    evs = send("");
    CHECK(ofKind(evs, PluginHostEvent::Kind::Badge) == 0);
}
