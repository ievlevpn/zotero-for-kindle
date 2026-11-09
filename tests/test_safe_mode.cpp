// Safe mode (src/plugins/safe_mode.h): plugins can't keep the app from starting.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "fake_nav.h"
#include "fake_plugins.h"
#include "fixture_library.h"
#include "screens/plugins.h"
#include "store/db.h"
#include "plugins/safe_mode.h"
#include "plugins/state.h"

using namespace zk;
using namespace zk::test;

namespace {

std::string startsFile(FakePlugins& d) { return d.dir() + "/.starts"; }

std::string readStarts(FakePlugins& d) {
    std::ifstream in(startsFile(d));
    std::string s;
    std::getline(in, s);
    return s;
}

// A plugin "a", turned on: starts count only while some plugin is on.
void enableOne(FakePlugins& d) {
    d.add("a", "");
    PluginState(d.dir() + "/.state.json").setEnabled("a", true);
}

}  // namespace

TEST_CASE("safe mode: two unconfirmed starts turn plugins off, automatic") {
    FakePlugins d;
    d.add("a", "");
    d.add("b", "");
    const std::string dir = d.dir();
    {
        PluginState st(dir + "/.state.json");
        st.setEnabled("a", true);
        st.setEnabled("b", true);
        st.recordFailure("b", "boom");
        st.stop("b");  // the app had stopped it: that note stays
    }
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);  // start 1
    CHECK(readStarts(d) == "1");
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);  // start 2: the first never got confirmed
    CHECK(readStarts(d) == "2");
    const auto third = decideSafeMode(dir, false);       // start 3
    CHECK(third.pluginsOff);
    CHECK(third.automatic);
    // Every plugin is off in the state: turning one back on is a deliberate act.
    PluginState st(dir + "/.state.json");
    CHECK_FALSE(st.enabled("a"));
    CHECK_FALSE(st.enabled("b"));
    CHECK(st.stopped("b"));
    CHECK(st.failures("b") == 1);
    CHECK(st.everEnabled("a"));
    // The count restarted: the start after the user turned a plugin on tries plugins again.
    CHECK(readStarts(d) == "0");
    st.setEnabled("a", true);
    const auto next = decideSafeMode(dir, false);
    CHECK_FALSE(next.pluginsOff);
    CHECK(st.enabled("a"));
}

TEST_CASE("safe mode: a confirmed start resets the count") {
    FakePlugins d;
    enableOne(d);
    const std::string dir = d.dir();
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
    confirmSafeModeStart(dir);
    CHECK(readStarts(d) == "0");
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
    confirmSafeModeStart(dir);
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
        confirmSafeModeStart(dir);
    }
}

TEST_CASE("safe mode: a garbled count reads as zero") {
    FakePlugins d;
    enableOne(d);
    d.write(".starts", "not a number");
    CHECK_FALSE(decideSafeMode(d.dir(), false).pluginsOff);
    CHECK(readStarts(d) == "1");
}

TEST_CASE("safe mode: OFF turns them off while it exists") {
    FakePlugins d;
    d.add("a", "");
    const std::string dir = d.dir();
    {
        PluginState st(dir + "/.state.json");
        st.setEnabled("a", true);
    }
    d.write("OFF", "");
    for (int i = 0; i < 4; ++i) {
        const auto s = decideSafeMode(dir, false);
        CHECK(s.pluginsOff);
        CHECK_FALSE(s.automatic);
    }
    CHECK(PluginState(dir + "/.state.json").enabled("a"));  // nothing persisted
    CHECK(readStarts(d).empty());                            // and no starts counted
    std::filesystem::remove(d.dir() + "/OFF");
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
}

TEST_CASE("safe mode: the second launcher's variable turns them off") {
    FakePlugins d;
    d.add("a", "");
    const std::string dir = d.dir();
    {
        PluginState st(dir + "/.state.json");
        st.setEnabled("a", true);
    }
    const auto s = decideSafeMode(dir, true);
    CHECK(s.pluginsOff);
    CHECK_FALSE(s.automatic);
    CHECK(PluginState(dir + "/.state.json").enabled("a"));
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);  // the next normal start is normal
}

TEST_CASE("safe mode: Settings -> Plugins lists the plugins without loading them and says why they are off") {
    FakePlugins d;
    d.add("a", "throw new Error('must not run')", {}, "Alpha");
    PluginHost host(d.deps());  // never started: nothing loads
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    int seen = 0;
    PluginsScreen list([&] { return &host; }, PluginsOff{"Plugins are off: the OFF file is in the plugins folder", [&] { ++seen; }});
    list.onShow(nav);
    REQUIRE(list.plugins().size() == 1);
    CHECK(list.plugins()[0].folder.dir == "a");
    CHECK_FALSE(list.plugins()[0].loaded);
    CHECK(seen == 1);
    CHECK(list.rowRects()[0].y > 200);  // the line about why sits above the rows
}

TEST_CASE("safe mode: the system ending the app counts as a confirmed start") {
    FakePlugins d;
    enableOne(d);
    const std::string dir = d.dir();
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
    armSafeModeSignalConfirm(dir);
    confirmSafeModeStartFromSignal();  // what the SIGTERM/SIGINT/SIGHUP handlers call
    CHECK_FALSE(decideSafeMode(dir, false).pluginsOff);
    CHECK(readStarts(d) == "1");  // back to counting from zero
    // (The handlers themselves, in main.cpp and wet_ink.cpp, only call this and re-raise: not run here.)
}

TEST_CASE("safe mode: with no plugin on, starts aren't counted and nothing is written") {
    SUBCASE("no plugins folder: none is created") {
        const std::string dir = (std::filesystem::temp_directory_path() / ("zk-no-plugins-" + std::to_string(::getpid()))).string();
        std::filesystem::remove_all(dir);
        for (int i = 0; i < 4; ++i) {
            const auto s = decideSafeMode(dir, false);
            CHECK_FALSE(s.pluginsOff);
            CHECK_FALSE(s.automatic);
            CHECK_FALSE(s.counting);
        }
        CHECK_FALSE(std::filesystem::exists(dir));
    }
    SUBCASE("plugins installed but all off: never automatic, no count file") {
        FakePlugins d;
        d.add("a", "");
        d.add("b", "");
        PluginState(d.dir() + "/.state.json").setEnabled("b", false);
        for (int i = 0; i < 4; ++i) {
            const auto s = decideSafeMode(d.dir(), false);
            CHECK_FALSE(s.pluginsOff);
            CHECK_FALSE(s.automatic);
            CHECK_FALSE(s.counting);
        }
        CHECK_FALSE(std::filesystem::exists(startsFile(d)));
    }
    SUBCASE("a count left from when a plugin was on is dropped, so turning one on later starts afresh") {
        FakePlugins d;
        d.add("a", "");
        d.write(".starts", "2\n");
        const auto s = decideSafeMode(d.dir(), false);
        CHECK_FALSE(s.pluginsOff);
        CHECK_FALSE(s.automatic);
        CHECK_FALSE(std::filesystem::exists(startsFile(d)));
        PluginState(d.dir() + "/.state.json").setEnabled("a", true);
        const auto on = decideSafeMode(d.dir(), false);
        CHECK_FALSE(on.pluginsOff);
        CHECK(on.counting);
        CHECK(readStarts(d) == "1");
    }
    SUBCASE("a plugin on in the state but no longer installed doesn't count") {
        FakePlugins d;
        PluginState(d.dir() + "/.state.json").setEnabled("gone", true);
        for (int i = 0; i < 3; ++i) CHECK_FALSE(decideSafeMode(d.dir(), false).automatic);
        CHECK_FALSE(std::filesystem::exists(startsFile(d)));
    }
}

TEST_CASE("safe mode: with a plugin on, the start is counted (counting = true)") {
    FakePlugins d;
    enableOne(d);
    const auto s = decideSafeMode(d.dir(), false);
    CHECK(s.counting);
    CHECK(readStarts(d) == "1");
}
