#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_http.h"
#include "fake_nav.h"
#include "fake_zotero.h"
#include "fixture_library.h"
#include "net/write_guard.h"
#include "screens/item_sheet.h"
#include "screens/read_tag.h"
#include "screens/reader.h"
#include "screens/settings.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/sync_engine.h"
#include "zotero/api.h"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

// A server with two books (one tagged "economics"), pulled once, every request through the guard
// as on the Kindle.
struct Rig {
    FakeZotero server;
    Db db{Db::open(":memory:")};
    Library lib{db};
    WriteGuard guard{server, [this](const std::string& k) { return lib.annotation(k).has_value(); },
                     [this](const std::string& k) { return lib.attachment(k).has_value(); },
                     [this](const std::string& k) -> std::optional<std::vector<std::string>> {
                         if (auto i = lib.zoteroItem(k)) return i->tags;
                         return std::nullopt;
                     },
                     pluginChangeShapes(lib)};
    ZoteroApi api{guard, 1, "k3y"};
    SyncEngine engine{api, lib};

    Rig() {
        api.setSleeper([](int) {});
        json one = FakeZotero::item("BOOK0001", "One");
        one["tags"] = json::array({{{"tag", "economics"}, {"type", 1}}});
        server.put(one);
        server.put(FakeZotero::item("BOOK0002", "Two"));
        REQUIRE_FALSE(engine.pull(nullptr).error);
    }
    std::vector<std::string> serverTags(const std::string& key) {
        std::vector<std::string> out;
        for (const auto& t : server.items[key]["data"]["tags"]) out.push_back(t["tag"]);
        return out;
    }
    int64_t unread() {
        ListQuery q;
        q.filter = Filter::Unread;
        return lib.countRows(q);
    }
};

}  // namespace

TEST_CASE("mark read: the read tag makes an item read, in rows, details and the Unread chip") {
    Rig rig;
    CHECK(rig.lib.readTag() == "read");
    CHECK(rig.unread() == 2);
    json two = rig.server.items["BOOK0002"]["data"];
    two["tags"] = json::array({{{"tag", "read"}}});
    rig.server.put(two);  // tagged on the desktop
    REQUIRE_FALSE(rig.engine.pull(nullptr).error);
    CHECK(rig.lib.rowFor("BOOK0002")->read);
    CHECK_FALSE(rig.lib.rowFor("BOOK0001")->read);
    CHECK(rig.lib.details("BOOK0002")->read);
    CHECK(rig.unread() == 1);
    // Another tag: what had only "read" isn't read any more, and nothing is retagged.
    rig.lib.setReadTag("economics");
    CHECK(rig.lib.rowFor("BOOK0001")->read);
    CHECK_FALSE(rig.lib.rowFor("BOOK0002")->read);
    CHECK(rig.lib.readMarks().empty());
    rig.lib.setReadTag("");
    CHECK(rig.lib.readTag() == "read");
}

TEST_CASE("mark read: a mark shows at once and is sent as one tag on the item") {
    Rig rig;
    rig.lib.markRead("BOOK0001", true);
    CHECK(rig.lib.isRead("BOOK0001"));
    CHECK(rig.lib.rowFor("BOOK0001")->read);
    CHECK(rig.unread() == 1);
    CHECK(rig.engine.pendingChanges() == 1);
    SyncReport rep = rig.engine.push();
    REQUIRE_FALSE(rep.error);
    CHECK(rig.serverTags("BOOK0001") == std::vector<std::string>{"economics", "read"});
    CHECK(rig.server.items["BOOK0001"]["data"]["tags"][0]["type"] == 1);  // the others kept as they were
    CHECK(rig.server.items["BOOK0001"]["data"]["title"] == "One");
    CHECK(rig.lib.readMarks().empty());
    CHECK(rig.lib.isRead("BOOK0001"));  // from the item's own tags now
    CHECK(rig.lib.item("BOOK0001")->version == rig.server.items["BOOK0001"]["version"]);
    // A pull of our own change keeps it read.
    REQUIRE_FALSE(rig.engine.pull(nullptr).error);
    CHECK(rig.lib.isRead("BOOK0001"));

    rig.lib.markRead("BOOK0001", false);
    CHECK_FALSE(rig.lib.isRead("BOOK0001"));
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.serverTags("BOOK0001") == std::vector<std::string>{"economics"});
    CHECK_FALSE(rig.lib.isRead("BOOK0001"));
}

TEST_CASE("mark read: a tag changed on the desktop meanwhile is kept (412, fetched, sent again)") {
    Rig rig;
    rig.lib.markRead("BOOK0001", true);
    json one = rig.server.items["BOOK0001"]["data"];
    one["tags"].push_back({{"tag", "favourite"}});
    rig.server.put(one);  // not pulled yet
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.serverTags("BOOK0001") == std::vector<std::string>{"economics", "favourite", "read"});
    CHECK(rig.lib.readMarks().empty());
}

TEST_CASE("mark read: an item deleted in Zotero meanwhile drops the mark without an error") {
    Rig rig;
    rig.lib.markRead("BOOK0002", true);
    rig.server.remove("BOOK0002");  // not pulled yet
    CHECK_FALSE(rig.engine.push().error);
    CHECK(rig.lib.readMarks().empty());
}

TEST_CASE("mark read: nothing is sent when Zotero already has it so") {
    Rig rig;
    rig.lib.markRead("BOOK0002", false);  // not read there either
    const size_t before = rig.server.sent.size();
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.server.count("PATCH", "/items/") == 0);
    CHECK(rig.server.sent.size() == before);
    CHECK(rig.lib.readMarks().empty());
}

TEST_CASE("mark read: the mark keeps the tag it was made with, and a newer mark survives the push of an older one") {
    Rig rig;
    rig.lib.markRead("BOOK0002", true);
    const ReadMark sent = rig.lib.readMarks().at(0);
    rig.lib.markRead("BOOK0002", false);  // changed its mind while the first was on its way
    rig.lib.clearReadMark(sent);
    REQUIRE(rig.lib.readMarks().size() == 1);
    CHECK_FALSE(rig.lib.readMarks()[0].read);
    rig.lib.setReadTag("done");
    CHECK(rig.lib.readMarks()[0].tag == "read");
}

TEST_CASE("write guard: an item's tags may change by one tag, against its version, and nothing else") {
    FakeHttp inner;
    std::map<std::string, std::vector<std::string>> items{{"BOOK0001", {"economics"}}};
    WriteGuard guard(
        inner, [](const std::string&) { return false; }, [](const std::string&) { return false; },
        [&](const std::string& k) -> std::optional<std::vector<std::string>> {
            if (items.count(k)) return items[k];
            return std::nullopt;
        },
        [&](const std::string& k) -> std::optional<PluginChangeShape> {  // BOOK0001 marked read, at version 7
            if (k != "BOOK0001") return std::nullopt;
            PluginChangeShape s;
            s.item = true;
            s.version = 7;
            s.readMarks = {{"read", true}};
            return s;
        });
    auto patch = [&](const std::string& key, const std::string& body, bool versioned = true) {
        HttpRequest r;
        r.method = "PATCH";
        r.url = "https://api.zotero.org/users/1/items/" + key;
        r.body = body;
        if (versioned) r.headers = {{"If-Unmodified-Since-Version", "7"}};
        return guard.send(r);
    };
    auto blocked = [](const HttpResponse& r) { return r.curlCode == -1; };
    inner.expect("PATCH", "https://api.zotero.org/users/1/items/BOOK0001", FakeHttp::status(204));
    CHECK(patch("BOOK0001", R"({"tags":[{"tag":"economics","type":1},{"tag":"read"}]})").status == 204);
    CHECK(blocked(patch("BOOK0001", R"({"tags":[{"tag":"economics"},{"tag":"read"}]})", false)));
    CHECK(blocked(patch("BOOK0001", R"({"tags":[{"tag":"read"}]})")));  // two changes
    CHECK(blocked(patch("BOOK0001", R"({"tags":[{"tag":"economics"}]})")));  // none
    CHECK(blocked(patch("BOOK0001", R"({"tags":[{"tag":"economics"},{"tag":"read"}],"title":"x"})")));
    CHECK(blocked(patch("BOOK0001", R"({"tags":[{"tag":"economics"},{"tag":"read","colour":1}]})")));
    CHECK(blocked(patch("BOOK0001", R"({"tags":[{"tag":"economics"},{"tag":""}]})")));
    CHECK(blocked(patch("OTHER001", R"({"tags":[{"tag":"read"}]})")));
    HttpRequest put;
    put.method = "PUT";
    put.url = "https://api.zotero.org/users/1/items/BOOK0001";
    put.body = R"({"tags":[{"tag":"economics"},{"tag":"read"}]})";
    put.headers = {{"If-Unmodified-Since-Version", "7"}};
    CHECK(blocked(guard.send(put)));  // a PUT replaces the whole item
    CHECK(inner.sent.size() == 1);
}

TEST_CASE("mark read: the item sheet marks and unmarks, and asks for a push") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 4);
    FakeNav nav(lib);
    const ItemRow row = *lib.rowFor("IT000002");
    ItemSheet sheet(row, *lib.details(row.key));
    const Rect area{0, 0, 1236, 1500};
    CHECK(sheet.onGesture(tapAt(sheet.readRect(area).center()), area, nav) == ItemSheet::Action::Changed);
    CHECK(lib.isRead("IT000002"));
    CHECK(sheet.row().read);
    CHECK(nav.syncFake.pushes == 1);
    CHECK(sheet.onGesture(tapAt(sheet.readRect(area).center()), area, nav) == ItemSheet::Action::Changed);
    CHECK_FALSE(lib.isRead("IT000002"));
}

TEST_CASE("mark read: the read tag screen keeps a typed tag or a tapped one; Back keeps the old") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 40);
    FakeNav nav(lib);
    {
        ReadTagScreen s;
        s.onShow(nav);
        CHECK(s.text() == "read");
        CHECK(nav.keyboardFake.shown);
        for (int i = 0; i < 4; ++i) s.onKey(KeyEvent{KeyEvent::Code::Backspace, ""}, nav);
        s.onKey(KeyEvent{KeyEvent::Code::Text, "done"}, nav);
        s.onGesture(tapAt(headerBackRect().center()), nav);
        CHECK(lib.readTag() == "read");
    }
    {
        ReadTagScreen s;
        s.onShow(nav);
        for (int i = 0; i < 4; ++i) s.onKey(KeyEvent{KeyEvent::Code::Backspace, ""}, nav);
        s.onKey(KeyEvent{KeyEvent::Code::Text, "done"}, nav);
        s.onKey(KeyEvent{KeyEvent::Code::Return, ""}, nav);
        CHECK(lib.readTag() == "done");
    }
    {
        ReadTagScreen s;
        s.onShow(nav);
        for (int i = 0; i < 4; ++i) s.onKey(KeyEvent{KeyEvent::Code::Backspace, ""}, nav);
        s.onKey(KeyEvent{KeyEvent::Code::Text, "ECON"}, nav);  // any case
        REQUIRE_FALSE(s.suggestions().empty());
        CHECK(s.suggestions()[0].first == "economics");
        s.onGesture(tapAt(s.suggestionRects()[0].center()), nav);
        CHECK(lib.readTag() == "economics");
    }
    {
        ReadTagScreen s;
        s.onShow(nav);
        for (int i = 0; i < 9; ++i) s.onKey(KeyEvent{KeyEvent::Code::Backspace, ""}, nav);
        s.onKey(KeyEvent{KeyEvent::Code::Return, ""}, nav);
        CHECK(lib.readTag() == "read");  // empty: the default again
    }
    SettingsScreen settings(Config{}, [](Nav&) {});
    settings.onShow(nav);
    const size_t pushed = nav.pushed.size();
    settings.onGesture(tapAt(settings.readTagRect().center()), nav);
    REQUIRE(nav.pushed.size() == pushed + 1);
    CHECK(dynamic_cast<ReadTagScreen*>(nav.pushed.back().get()));
}

TEST_CASE("mark read: turning on past the last page offers Mark as read") {
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 10);
    FakeNav nav(lib);
    const ItemRow row = *lib.rowFor("IT000003");
    ReaderScreen r(row, "book.epub", DocType::Epub, [] {
        return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
    }, 6);
    auto waitPage = [&](int page) {
        for (int i = 0; i < 400; ++i) {
            r.onTick(nav);
            if (r.page() == page && r.ready()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    r.onShow(nav);
    REQUIRE(waitPage(0));
    const int last = r.pageCount() - 1;
    r.goToPage(last);
    REQUIRE(waitPage(last));
    CHECK_FALSE(r.endPrompt());
    r.onGesture(swipe(Direction::Left), nav);
    REQUIRE(r.endPrompt());
    r.onGesture(swipe(Direction::Left), nav);  // any gesture closes it
    CHECK_FALSE(r.endPrompt());
    CHECK_FALSE(lib.isRead("IT000003"));
    r.onGesture(tapAt({1100, 700}), nav);
    REQUIRE(r.endPrompt());
    r.onGesture(tapAt(r.endPromptButtonRect().center()), nav);
    CHECK_FALSE(r.endPrompt());
    CHECK(lib.isRead("IT000003"));
    CHECK(nav.syncFake.pushes == 1);
    r.onGesture(swipe(Direction::Left), nav);  // read already: no prompt
    CHECK_FALSE(r.endPrompt());
}

TEST_CASE("mark read: a read row says Read where the progress was") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Db db = Db::open(":memory:");
    Library lib(db);
    fillFixtureLibrary(lib, 4);
    lib.markOpened("IT000001", "EP000001", 1);
    lib.saveReading("IT000001", "", 0.6);
    lib.markRead("IT000001", true);
    Canvas c(1236, 200);
    drawItemRow(c.cr(), {0, 0, 1236, 186}, *lib.rowFor("IT000001"), "");
    if (std::getenv("ZK_ROW_PNG")) cairo_surface_write_to_png(c.surface(), std::getenv("ZK_ROW_PNG"));
    int ink = 0;  // the right column's bottom line holds dark pixels ("✓ Read"), not the 60%
    unsigned char* px = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (int y = 90; y < 180; ++y)
        for (int x = 1236 - 160; x < 1236; ++x) ink += px[y * stride + x * 4] < 80;
    CHECK(ink > 100);
}
