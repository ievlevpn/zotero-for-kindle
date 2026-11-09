// Task 15: background tasks (after a successful sync, at most hourly, inside a keep-awake hold)
// and badges.

#include <doctest/doctest.h>

#include <atomic>

#include "app/plugin_background.h"
#include "fake_lipc.h"
#include "fake_plugins.h"
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
    BackgroundRound round(awake, [&] { ++runs; }, [&] { ++cancels; });

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
    round.done();
    CHECK_FALSE(round.holding());
    CHECK(lipc.sets.back() == "com.lab126.powerd preventScreenSaver 0");
    round.done();  // a stray report changes nothing
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
    round.onSleep();  // nothing running: nothing to cut
    CHECK(cancels == 1);
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
