// Task 16: zk.selectionAction / zk.itemAction / zk.screen are checked and recorded (part 2 shows them).

#include <doctest/doctest.h>

#include <filesystem>

#include "fake_plugins.h"
#include "plugins/host.h"

using namespace zk;
using namespace zk::test;

namespace {

// Loads one plugin whose main.js is `js`; returns its status.
struct Loaded {
    FakePlugins fp;
    PluginHost host;
    explicit Loaded(const std::string& js) : host(init(js)) {
        host.setEnabled("p", true);
        host.start();
        host.dispatch({"syncFinished", {}, false});
        host.waitIdle(3000);
    }
    PluginHostDeps init(const std::string& js) {
        fp.add("p", js);
        return fp.deps();
    }
    ~Loaded() { host.stop(); }
    PluginStatus status() const {
        const auto all = host.status();
        return *statusOf(all, "p");
    }
};

}  // namespace

TEST_CASE("entry: selectionAction/itemAction/screen validate their arguments (TypeError naming the field)") {
    // Each line must throw a TypeError whose message contains the second string.
    struct Case { const char* code; const char* mentions; };
    const Case cases[] = {
        {"zk.selectionAction()", "zk.selectionAction"},
        {"zk.selectionAction('x')", "zk.selectionAction"},
        {"zk.selectionAction({title:'T',run(){}})", "`id`"},
        {"zk.selectionAction({id:'Bad Id',title:'T',run(){}})", "`id`"},
        {"zk.selectionAction({id:'-a',title:'T',run(){}})", "`id`"},
        {"zk.selectionAction({id:'a',run(){}})", "`title`"},
        {"zk.selectionAction({id:'a',title:'  ',run(){}})", "`title`"},
        {"zk.selectionAction({id:'a',title:5,run(){}})", "`title`"},
        {"zk.selectionAction({id:'a',title:'x'.repeat(41),run(){}})", "`title`"},
        {"zk.selectionAction({id:'a',title:'T'})", "`run`"},
        {"zk.selectionAction({id:'a',title:'T',run:5})", "`run`"},
        {"zk.itemAction({id:'a',title:'T'})", "zk.itemAction"},
        {"zk.itemAction({id:'',title:'T',run(){}})", "`id`"},
        {"zk.screen()", "`id`"},
        {"zk.screen('Bad','x')", "`id`"},
        {"zk.screen('ok')", "`render`"},
        {"zk.screen('ok', 3)", "`render`"},
        {"zk.selectionAction({id:'a',title:'T',run(){}}); zk.selectionAction({id:'a',title:'U',run(){}})", "\"a\""},
        {"zk.screen('s',()=>1); zk.screen('s',()=>2)", "\"s\""},
    };
    for (const auto& c : cases) {
        CAPTURE(c.code);
        const std::string js = std::string("let e = null; try { ") + c.code +
                               "; } catch (x) { e = x; }\n"
                               "if (!(e instanceof TypeError)) throw new Error('not a TypeError: ' + e);\n"
                               "console.log('MSG ' + e.message);\n";
        Loaded l(js);
        const std::string log = l.fp.log("p");
        CHECK(log.find("MSG ") != std::string::npos);
        CHECK(log.find(c.mentions) != std::string::npos);
        const bool duplicate = std::string(c.code).find("; zk.") != std::string::npos;
        CHECK(l.status().entries.size() == (duplicate ? 1u : 0u));  // the first stands
        CHECK(l.status().errors.empty());
    }
    // The same id in different kinds is fine.
    Loaded l("zk.selectionAction({id:'a',title:'T',run(){}}); zk.itemAction({id:'a',title:'T',run(){}}); zk.screen('a',()=>1);");
    CHECK(l.status().entries.size() == 3);
}

TEST_CASE("entry: registrations are listed in PluginStatus for part 2") {
    Loaded l(
        "zk.selectionAction({id:'explain',title:'  Ex\\nplain ',run:()=>'x'});\n"
        "zk.itemAction({id:'export',title:'Export',run(){}});\n"
        "zk.screen('cards',()=>({}));\n");
    const auto st = l.status();
    REQUIRE(st.loaded);
    REQUIRE(st.entries.size() == 3);
    CHECK(st.entries[0].kind == "selectionAction");
    CHECK(st.entries[0].id == "explain");
    CHECK(st.entries[0].title == "Ex plain");
    CHECK(st.entries[1].kind == "itemAction");
    CHECK(st.entries[1].id == "export");
    CHECK(st.entries[1].title == "Export");
    CHECK(st.entries[2].kind == "screen");
    CHECK(st.entries[2].id == "cards");
    CHECK(st.entries[2].title.empty());

    // Dropped on unload, back after a reload (main.js runs again).
    l.host.setEnabled("p", false);
    l.host.waitIdle(3000);
    CHECK(l.status().entries.empty());
    l.host.setEnabled("p", true);
    l.host.dispatch({"syncFinished", {}, false});
    l.host.waitIdle(3000);
    CHECK(eventually([&] { return l.status().entries.size() == 3; }));
    l.host.reload("p");  // a restart: dropped and registered again, not doubled
    l.host.waitIdle(3000);
    CHECK(eventually([&] { return l.status().entries.size() == 3; }));
}

TEST_CASE("entry: Explain with AI (example) loads without error") {
    namespace fs = std::filesystem;
    FakePlugins fp;
    fs::copy("tests/data/plugins/examples/explain-with-ai", fp.path("explain-with-ai"), fs::copy_options::recursive);
    PluginHost host(fp.deps());
    host.setEnabled("explain-with-ai", true);
    host.start();
    host.dispatch({"syncFinished", {}, false});
    host.waitIdle(3000);
    const auto all = host.status();
    const PluginStatus* s = statusOf(all, "explain-with-ai");
    REQUIRE(s);
    CAPTURE(joined(s->errors));
    CHECK(s->errors.empty());
    CHECK(s->loaded);
    REQUIRE(s->entries.size() == 1);
    CHECK(s->entries[0].kind == "selectionAction");
    CHECK(s->entries[0].id == "explain");
    CHECK(s->entries[0].title == "Explain");
    host.stop();
}
