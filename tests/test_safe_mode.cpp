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
