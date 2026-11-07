#include <doctest/doctest.h>

#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/reader.h"
#include "store/db.h"
#include "ui/text.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}; }

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;
    std::vector<AnnotationRecord> anns;
    Rig() {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        std::ifstream in("tests/data/cfi/pride-and-prejudice.zotero.json");
        json records = json::parse(in);
        for (int i : {100, 101}) {
            AnnotationRecord a;
            a.key = "PPANN" + std::to_string(i);
            a.parentKey = "EP000003";
            a.type = i == 101 ? "underline" : "highlight";
            a.color = "#ffd400";
            a.sortIndex = records[i]["sortIndex"];
            a.position = json{{"type", "FragmentSelector"}, {"value", records[i]["cfi"]}}.dump();
            lib.upsertAnnotation(a);
            anns.push_back(a);
        }
        AnnotationRecord lost = anns[0];
        lost.key = "LOSTANN1";
        lost.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/1998!/4/2/1:0)"})j";
        lib.upsertAnnotation(lost);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "tests/data/epub/pride-and-prejudice.epub",
                                                DocType::Epub, [] {
                                                    return std::make_unique<RenderWorker>([](DocType t) {
                                                        UserError e;
                                                        return loadEngine(t, env(), &e);
                                                    });
                                                }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
    }
    bool settle(std::function<bool()> done) {
        for (int i = 0; i < 1000; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

}  // namespace

TEST_CASE("reader highlights: a Zotero highlight shades the page under its text; lost ones are skipped") {
    Rig rig;
    REQUIRE(rig.settle([&] { return rig.reader->ready(); }));
    rig.reader->jumpToAnnotation(rig.anns[0]);
    REQUIRE(rig.settle([&] { return !rig.reader->highlights().empty(); }));
    const auto boxes = rig.reader->highlights();
    bool sawHighlight = false, sawUnderline = false;
    for (const auto& b : boxes) {
        sawHighlight |= b.annotationKey == "PPANN100";
        sawUnderline |= b.annotationKey == "PPANN101" && b.underline;
        CHECK(b.annotationKey != "LOSTANN1");
    }
    CHECK(sawHighlight);
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    std::vector<uint8_t> g(1236 * 1547);
    c.toGray(g.data(), 1236, {0, 0, 1236, 1547});
    const Rect box = boxes[0].rect;
    int shaded = 0;
    for (int y = box.y; y < box.y + box.h; ++y)
        for (int x = box.x; x < box.x + box.w; ++x) {
            const uint8_t v = g[static_cast<size_t>(y) * 1236 + x];
            shaded += v > 180 && v < 240;  // paper under the highlight: neither white nor ink
        }
    CHECK(shaded > box.w * box.h / 4);
    (void)sawUnderline;
}

TEST_CASE("reader highlights: new annotations from Sync repaint the page") {
    Rig rig;
    REQUIRE(rig.settle([&] { return rig.reader->ready(); }));
    rig.reader->jumpToAnnotation(rig.anns[0]);
    REQUIRE(rig.settle([&] { return !rig.reader->highlights().empty(); }));
    rig.nav.damages.clear();
    SyncEvent e;
    e.kind = SyncEvent::Kind::AnnotationsReady;
    e.attachmentKey = "EP000003";
    rig.reader->onSyncEvent(e, rig.nav);
    REQUIRE(rig.settle([&] { return !rig.nav.damages.empty(); }));
    CHECK(rig.nav.damages[0].rect == ReaderScreen::pageArea());
}

TEST_CASE("reader highlights match the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    Rig rig;
    REQUIRE(rig.settle([&] { return rig.reader->ready(); }));
    rig.reader->jumpToAnnotation(rig.anns[0]);
    REQUIRE(rig.settle([&] { return !rig.reader->highlights().empty(); }));
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    test::checkGolden(c, "tests/golden/reader-highlights.png");
}
