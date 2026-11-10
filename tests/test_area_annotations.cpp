// Area annotations (Zotero `image`) on PDF pages: drawn with a long press and drag where there's
// no text, from the menus ("Mark an area"), or with the Scribe pen's side button held; then the
// edit bar (colour, Comment, Delete). Synced as Zotero's image annotations.

#include <doctest/doctest.h>

#include "app/app.h"
#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_display.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_sync.h"
#include "fake_zotero.h"
#include "fixture_library.h"
#include "golden.h"
#include "net/write_guard.h"
#include "screens/annotation_bar.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "sync/sync_engine.h"
#include "ui/gesture.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "zotero/api.h"
#include "zotero/parse.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

// FakeEngine's text: lines of words from {100, 200} to {1100, 680}. Below it, nothing.
constexpr Point kBlank{300, 800}, kBlankFar{700, 1100};
constexpr Point kOnText{130, 210};

PenEvent pen(PenEvent::Type type, Point p, bool button = false) {
    PenEvent e;
    e.type = type;
    e.pos = p;
    e.button = button;
    return e;
}

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    ItemRow row;
    std::unique_ptr<ReaderScreen> reader;

    explicit Rig(DocType type = DocType::Pdf, int page = 0, Rect box = {}) {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        for (auto& a : lib.annotationsFor("EP000003")) lib.removeObject("annotation", a.key);
        row = *lib.rowFor("IT000003");
        reader = std::make_unique<ReaderScreen>(row, type == DocType::Pdf ? "book.pdf" : "book.epub", type, [box] {
            return std::make_unique<RenderWorker>([box](DocType) {
                auto e = std::make_unique<FakeEngine>();
                e->box = box;
                return std::unique_ptr<DocumentEngine>(std::move(e));
            });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitFor([&] { return reader->ready(); }));
        if (page) {
            reader->turnTo(page);
            REQUIRE(waitFor([&] { return reader->page() == page; }));
        }
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void settle(int ms = 100) {
        for (int i = 0; i < ms / 5; ++i) {
            reader->onTick(nav);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void gesture(GestureType t, Point p, Point start) { reader->onGesture(Gesture{t, p, start}, nav); }
    void tap(Point p) { reader->onGesture(tapAt(p), nav); }
    // A long press at `from`, a drag to `to`, and the finger lifted.
    void longPressDrag(Point from, Point to, bool lift = true) {
        gesture(GestureType::LongPress, from, from);
        gesture(GestureType::DragStart, {(from.x + to.x) / 2, (from.y + to.y) / 2}, from);
        gesture(GestureType::DragMove, to, from);
        if (lift) gesture(GestureType::DragEnd, to, from);
    }
    std::vector<AnnotationRecord> areas() {
        std::vector<AnnotationRecord> out;
        for (auto& a : lib.annotationsFor(row.attachmentKey))
            if (a.type == "image") out.push_back(a);
        return out;
    }
    AnnotationEditor* editor() { return reader->annotationEditor(); }
};

json fixture(const std::string& name) {
    std::ifstream in("tests/data/zotero/" + name);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

std::unique_ptr<DocumentEngine> samplePdf() {
    UserError err;
    auto e = loadEngine(DocType::Pdf, EngineEnv{}, &err);
    REQUIRE(e);
    REQUIRE(e->open("tests/data/pdf/sample.pdf", &err));
    return e;
}

}  // namespace

// ---- Making one with a finger --------------------------------------------------------------

TEST_CASE("area: a long press where there's no text, then a drag, draws a rectangle that follows the finger") {
    Rig rig;
    rig.longPressDrag(kBlank, {500, 950}, false);
    REQUIRE(rig.waitFor([&] { return rig.reader->drawnArea().has_value(); }));
    CHECK_FALSE(rig.reader->selection());
    CHECK(*rig.reader->drawnArea() == Rect{300, 800, 200, 150});
    rig.nav.damages.clear();
    rig.gesture(GestureType::DragMove, kBlankFar, kBlank);
    CHECK(*rig.reader->drawnArea() == Rect{300, 800, 400, 300});
    // Only the frame's edges are repainted, fast.
    REQUIRE_FALSE(rig.nav.damages.empty());
    for (const auto& d : rig.nav.damages) {
        CHECK(d.waveform == Waveform::Fast);
        CHECK((d.rect.w < 40 || d.rect.h < 40));
    }
    // Lifting saves it in the colour used last, and the edit bar opens on it.
    rig.gesture(GestureType::DragEnd, kBlankFar, kBlank);
    REQUIRE(rig.waitFor([&] { return rig.editor() != nullptr; }));
    CHECK_FALSE(rig.reader->drawnArea());
    auto areas = rig.areas();
    REQUIRE(areas.size() == 1);
    CHECK(areas[0].color == "#ffd400");
    CHECK(areas[0].text.empty());
    CHECK(areas[0].pageLabel == "0");
    CHECK(areas[0].dirty);
    CHECK(areas[0].createdHere);
    CHECK(json::parse(areas[0].position)["rect"] == json::array({300, 800, 400, 300}));
    CHECK(rig.editor()->key() == areas[0].key);
    CHECK(rig.editor()->bar().mode() == AnnotationBar::Mode::Edit);
    CHECK(rig.nav.syncFake.pushes >= 1);
    REQUIRE(rig.waitFor([&] {
        for (const auto& b : rig.reader->highlights())
            if (b.kind == HighlightBox::Kind::Image && b.annotationKey == areas[0].key) return true;
        return false;
    }));
}

TEST_CASE("area: a long press on text is still a text selection") {
    Rig rig;
    rig.longPressDrag(kOnText, {650, 330});
    REQUIRE(rig.waitFor([&] { return rig.reader->annotationBar() != nullptr; }));
    CHECK(rig.reader->selection());
    CHECK(rig.reader->annotationBar()->mode() == AnnotationBar::Mode::Create);
    CHECK_FALSE(rig.reader->drawnArea());
    CHECK(rig.areas().empty());
}

TEST_CASE("area: the drag may end before the reader knows the press wasn't on text") {
    Rig rig;
    rig.longPressDrag(kBlank, kBlankFar);  // all of it at once, before any answer
    REQUIRE(rig.waitFor([&] { return rig.editor() != nullptr; }));
    REQUIRE(rig.areas().size() == 1);
    CHECK(json::parse(rig.areas()[0].position)["rect"] == json::array({300, 800, 400, 300}));
}

TEST_CASE("area: a rectangle smaller than about 5 mm on either side is ignored") {
    Rig rig;
    rig.longPressDrag(kBlank, {300 + 400, 800 + ReaderScreen::kMinAreaPx - 5});
    rig.settle();
    CHECK(rig.areas().empty());
    CHECK_FALSE(rig.editor());
    CHECK_FALSE(rig.reader->drawnArea());
    // A long press without a drag makes nothing either.
    rig.gesture(GestureType::LongPress, kBlank, kBlank);
    rig.gesture(GestureType::LongPressUp, kBlank, kBlank);
    rig.settle();
    CHECK(rig.areas().empty());
    // Just big enough is kept.
    rig.longPressDrag(kBlank, {300 + ReaderScreen::kMinAreaPx, 800 + ReaderScreen::kMinAreaPx});
    REQUIRE(rig.waitFor([&] { return rig.areas().size() == 1; }));
}

TEST_CASE("area: not offered in an EPUB (Zotero has no image annotations there)") {
    Rig rig(DocType::Epub);
    rig.longPressDrag(kBlank, kBlankFar);
    rig.settle();
    CHECK_FALSE(rig.reader->drawnArea());
    CHECK(rig.areas().empty());
    CHECK_FALSE(rig.editor());
}

TEST_CASE("area: the colour picked on its bar becomes the one used next") {
    Rig rig;
    rig.longPressDrag(kBlank, kBlankFar);
    REQUIRE(rig.waitFor([&] { return rig.editor() != nullptr; }));
    rig.tap(rig.editor()->bar().dotRect(3).center());  // Blue
    CHECK(rig.areas()[0].color == "#2ea8e5");
    rig.tap({600, 1400});  // closes the bar
    CHECK_FALSE(rig.editor());
    rig.longPressDrag({300, 1200}, {700, 1400});
    REQUIRE(rig.waitFor([&] { return rig.areas().size() == 2; }));
    for (const auto& a : rig.areas()) CHECK(a.color == "#2ea8e5");
}

// ---- Selecting one, deleting it ------------------------------------------------------------

namespace {

AnnotationRecord savedArea(const std::string& key, Rect r, const std::string& page = "0") {
    AnnotationRecord a;
    a.key = key;
    a.parentKey = "EP000003";
    a.type = "image";
    a.color = "#5fb236";
    a.pageLabel = page;
    a.version = 7;
    a.position = json{{"fake", std::stoi(page)}, {"rect", {r.x, r.y, r.w, r.h}}}.dump();
    return a;
}

}  // namespace

TEST_CASE("area: tapping near an existing area's frame, inside or out, selects it with the edit bar") {
    Rig rig;
    rig.lib.upsertAnnotation(savedArea("AREA0001", {300, 800, 400, 300}));
    SyncEvent ready;
    ready.kind = SyncEvent::Kind::AnnotationsReady;
    ready.attachmentKey = "EP000003";
    rig.reader->onSyncEvent(ready, rig.nav);
    REQUIRE(rig.waitFor([&] { return !rig.reader->highlights().empty(); }));
    CHECK(rig.reader->highlights()[0].kind == HighlightBox::Kind::Image);
    for (Point p : {Point{301, 900}, Point{360, 950}, Point{699, 1099}, Point{500, 1150}, Point{250, 950}}) {
        rig.tap(p);
        REQUIRE(rig.editor());
        CHECK(rig.editor()->key() == "AREA0001");
        CHECK(rig.editor()->bar().colour() == "#5fb236");
        CHECK_FALSE(rig.reader->menusOpen());
        rig.tap({600, 1400});
        CHECK_FALSE(rig.editor());
    }
}

TEST_CASE("area: a tap in the middle of a large area is the page's; a small area is selectable anywhere") {
    Rig rig;
    rig.lib.upsertAnnotation(savedArea("AREABIG1", {250, 300, 750, 900}));
    rig.lib.upsertAnnotation(savedArea("AREASML1", {150, 1250, 150, 150}));
    SyncEvent ready;
    ready.kind = SyncEvent::Kind::AnnotationsReady;
    ready.attachmentKey = "EP000003";
    rig.reader->onSyncEvent(ready, rig.nav);
    REQUIRE(rig.waitFor([&] { return rig.reader->highlights().size() == 2; }));
    rig.tap({618, 750});  // the big area's centre: the menus
    CHECK_FALSE(rig.editor());
    CHECK(rig.reader->menusOpen());
    rig.tap({618, 1000});  // closes them
    REQUIRE_FALSE(rig.reader->menusOpen());
    rig.tap({618, 300 + ReaderScreen::kAreaFramePx - 5});  // near its top edge, inside
    REQUIRE(rig.editor());
    CHECK(rig.editor()->key() == "AREABIG1");
    rig.tap({618, 1450});
    REQUIRE_FALSE(rig.editor());
    rig.tap({225, 1325});  // the small one's centre, 75 px from its frame
    REQUIRE(rig.editor());
    CHECK(rig.editor()->key() == "AREASML1");
    rig.tap({618, 1450});
    // Where both qualify, the nearest frame wins: just below the big one, near the small one's top.
    rig.tap({225, 1240});  // 41 px from the big one, 10 from the small one
    REQUIRE(rig.editor());
    CHECK(rig.editor()->key() == "AREASML1");
}

TEST_CASE("area: Delete asks about the area, then deletes it through the usual deletion") {
    Rig rig;
    rig.lib.upsertAnnotation(savedArea("AREA0001", {300, 800, 400, 300}));
    SyncEvent ready;
    ready.kind = SyncEvent::Kind::AnnotationsReady;
    ready.attachmentKey = "EP000003";
    rig.reader->onSyncEvent(ready, rig.nav);
    REQUIRE(rig.waitFor([&] { return !rig.reader->highlights().empty(); }));
    rig.tap({310, 950});  // near its frame
    REQUIRE(rig.editor());
    rig.tap(rig.editor()->bar().buttonRect(1).center());  // Delete
    REQUIRE(rig.editor()->confirming());
    CHECK(rig.editor()->question() == "Delete this area?");
    rig.tap(rig.editor()->confirmRect(0).center());
    CHECK_FALSE(rig.editor());
    REQUIRE(rig.lib.pendingDeletions().size() == 1);
    CHECK(rig.lib.pendingDeletions()[0].key == "AREA0001");
    CHECK(rig.areas().empty());
    REQUIRE(rig.waitFor([&] { return rig.reader->highlights().empty(); }));
}

TEST_CASE("area: Comment on a new area saves its comment") {
    Rig rig;
    rig.longPressDrag(kBlank, kBlankFar);
    REQUIRE(rig.waitFor([&] { return rig.editor() != nullptr; }));
    rig.tap(rig.editor()->bar().buttonRect(0).center());  // Comment
    rig.reader->onKey(KeyEvent{KeyEvent::Code::Text, "figure 3"}, rig.nav);
    rig.reader->onKey(KeyEvent{KeyEvent::Code::Return, ""}, rig.nav);
    CHECK(rig.areas()[0].comment == "figure 3");
}

// ---- The menus: Mark an area ---------------------------------------------------------------

TEST_CASE("area: Mark an area in the menus (PDF only); the next drag draws one, then the mode ends") {
    Rig rig;
    rig.tap({618, 700});
    REQUIRE(rig.reader->menusOpen());
    REQUIRE_FALSE(ReaderMenus::markAreaRect().empty());
    rig.tap(ReaderMenus::markAreaRect().center());
    CHECK_FALSE(rig.reader->menusOpen());
    CHECK(rig.reader->areaArmed());
    CHECK(rig.reader->dragsAtOnce());  // a plain drag is a drag, no long press needed
    // A drag over text: an area all the same.
    rig.gesture(GestureType::DragStart, {200, 260}, kOnText);
    REQUIRE(rig.reader->drawnArea());
    CHECK(*rig.reader->drawnArea() == Rect{130, 210, 70, 50});
    rig.gesture(GestureType::DragMove, {900, 500}, kOnText);
    rig.gesture(GestureType::DragEnd, {900, 500}, kOnText);
    CHECK_FALSE(rig.reader->areaArmed());
    CHECK_FALSE(rig.reader->dragsAtOnce());
    REQUIRE(rig.waitFor([&] { return rig.editor() != nullptr; }));
    REQUIRE(rig.areas().size() == 1);
    CHECK(json::parse(rig.areas()[0].position)["rect"] == json::array({130, 210, 770, 290}));
    CHECK_FALSE(rig.reader->selection());
}

TEST_CASE("area: Mark an area waits through a slip and ends on Cancel") {
    Rig rig;
    rig.tap({618, 700});
    rig.tap(ReaderMenus::markAreaRect().center());
    REQUIRE(rig.reader->areaArmed());
    rig.nav.damages.clear();
    // A slip (too small): ignored, still waiting.
    rig.gesture(GestureType::DragStart, {420, 820}, {400, 800});
    rig.gesture(GestureType::DragEnd, {420, 820}, {400, 800});
    rig.settle();
    CHECK(rig.areas().empty());
    CHECK(rig.reader->areaArmed());
    // Other taps don't open the menus or turn the page meanwhile.
    rig.tap({618, 700});
    rig.tap({1150, 700});
    rig.settle();
    CHECK_FALSE(rig.reader->menusOpen());
    CHECK(rig.reader->page() == 0);
    CHECK(rig.reader->areaArmed());
    rig.tap(ReaderScreen::areaCancelRect().center());
    CHECK_FALSE(rig.reader->areaArmed());
    rig.tap({618, 700});
    CHECK(rig.reader->menusOpen());
}

TEST_CASE("area: the menus offer Mark an area only on PDFs") {
    Rig rig(DocType::Epub);
    rig.tap({618, 700});
    REQUIRE(rig.reader->menusOpen());
    rig.tap(ReaderMenus::markAreaRect().center());
    CHECK_FALSE(rig.reader->areaArmed());
}

TEST_CASE("area: a screen that asks for drags at once gets a drag without a long press") {
    GestureRecognizer g;
    g.setDragAtOnce(true);
    std::vector<Gesture> out;
    for (auto& x : g.feed({TouchEvent::Type::Down, 0, {100, 100}, 0})) out.push_back(x);
    for (auto& x : g.feed({TouchEvent::Type::Move, 0, {200, 180}, 40})) out.push_back(x);
    for (auto& x : g.feed({TouchEvent::Type::Move, 0, {400, 300}, 2000})) out.push_back(x);
    for (auto& x : g.feed({TouchEvent::Type::Up, 0, {400, 300}, 2100})) out.push_back(x);
    REQUIRE(out.size() == 4);
    CHECK(out[0].type == GestureType::DragStart);
    CHECK(out[1].type == GestureType::DragMove);
    CHECK(out[2].type == GestureType::DragMove);
    CHECK(out[3].type == GestureType::DragEnd);
    CHECK(out[3].start == Point{100, 100});
    // A tap is still a tap; and without the flag a quick drag is a swipe, as before.
    out.clear();
    for (auto& x : g.feed({TouchEvent::Type::Down, 0, {100, 100}, 3000})) out.push_back(x);
    for (auto& x : g.feed({TouchEvent::Type::Up, 0, {104, 100}, 3050})) out.push_back(x);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Tap);
    g.setDragAtOnce(false);
    out.clear();
    for (auto& x : g.feed({TouchEvent::Type::Down, 0, {100, 100}, 4000})) out.push_back(x);
    for (auto& x : g.feed({TouchEvent::Type::Move, 0, {400, 100}, 4040})) out.push_back(x);
    for (auto& x : g.feed({TouchEvent::Type::Up, 0, {400, 100}, 4080})) out.push_back(x);
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == GestureType::Swipe);
}

namespace {

class NoKeyboard : public Keyboard {
public:
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

struct DragScreen : Screen {
    bool atOnce = true;
    std::vector<GestureType> got;
    void draw(Canvas&) override {}
    void onGesture(const Gesture& g, Nav&) override { got.push_back(g.type); }
    bool dragsAtOnce() const override { return atOnce; }
};

}  // namespace

TEST_CASE("area: the app gives a screen that asks for it drags at once") {
    FakeDisplay display;
    FakeSync sync;
    NoKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    App app{display, lib, sync, keyboard, [&] { return now; }};
    auto s = std::make_unique<DragScreen>();
    DragScreen* screen = s.get();
    app.setRoot(0, std::move(s));
    app.start();
    display.touch(TouchEvent::Type::Down, {300, 300}, now);
    display.touch(TouchEvent::Type::Move, {500, 500}, now + 30);
    display.touch(TouchEvent::Type::Up, {500, 500}, now + 60);
    for (int i = 0; i < 4; ++i) app.step(0);
    REQUIRE(screen->got.size() >= 2);
    CHECK(screen->got.front() == GestureType::DragStart);
    CHECK(screen->got.back() == GestureType::DragEnd);
}

// ---- The pen's side button (Scribe) --------------------------------------------------------

TEST_CASE("area: with Mark an area on, the pen draws the area without the button") {
    Rig rig;
    rig.tap({618, 700});
    rig.tap(ReaderMenus::markAreaRect().center());
    REQUIRE(rig.reader->areaArmed());
    CHECK(rig.reader->onPen(pen(PenEvent::Type::Down, kBlank), rig.nav));
    rig.reader->onPen(pen(PenEvent::Type::Move, kBlankFar), rig.nav);
    rig.reader->onPen(pen(PenEvent::Type::Up, kBlankFar), rig.nav);
    CHECK(rig.reader->pendingInkStrokes() == 0);
    REQUIRE(rig.waitFor([&] { return rig.areas().size() == 1; }));
    CHECK_FALSE(rig.reader->areaArmed());
}

// ---- PDF positions and sync ----------------------------------------------------------------

TEST_CASE("area: a PDF area is Zotero's image position in PDF points, and is drawn back where it was drawn") {
    auto pdf = samplePdf();
    const Rect drawn{200, 400, 500, 300};
    auto placed = pdf->areaPosition(1, drawn);
    REQUIRE(placed);
    const json pos = json::parse(placed->position);
    CHECK(pos["pageIndex"] == 1);
    REQUIRE(pos["rects"].size() == 1);
    REQUIRE(pos["rects"][0].size() == 4);
    const auto r = pos["rects"][0];
    CHECK(r[0].get<double>() < r[2].get<double>());  // x1 < x2, y1 < y2: PDF user space, origin bottom-left
    CHECK(r[1].get<double>() < r[3].get<double>());
    CHECK(pos.size() == 2);  // nothing but pageIndex and rects
    // Sort index as for highlights: page | offset | top.
    REQUIRE(placed->sortIndex.size() == 18);
    CHECK(placed->sortIndex.rfind("00001|", 0) == 0);
    CHECK(placed->sortIndex[12] == '|');
    CHECK(placed->pageLabel == "2");
    AnnotationRecord a;
    a.key = "IMAGRT01";
    a.type = "image";
    a.color = "#ffd400";
    a.position = placed->position;
    auto boxes = pdf->highlights(1, {a});
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].kind == HighlightBox::Kind::Image);
    CHECK(std::abs(boxes[0].rect.x - drawn.x) <= 1);
    CHECK(std::abs(boxes[0].rect.y - drawn.y) <= 1);
    CHECK(std::abs(boxes[0].rect.w - drawn.w) <= 2);
    CHECK(std::abs(boxes[0].rect.h - drawn.h) <= 2);
    // Lower on the page sorts later.
    auto lower = pdf->areaPosition(1, {200, 900, 500, 300});
    REQUIRE(lower);
    CHECK(lower->sortIndex > placed->sortIndex);
}

TEST_CASE("area: an EPUB has no areas") {
    FakeEngine plain;
    DocumentEngine& base = plain;
    CHECK_FALSE(base.DocumentEngine::areaPosition(0, {0, 0, 100, 100}));
}

TEST_CASE("area: one made in Zotero desktop parses, and shows as its area on the page") {
    const AnnotationRecord a = parseAnnotation(fixture("annotation_image.json"));
    CHECK(a.type == "image");
    CHECK(a.text.empty());
    CHECK(a.comment == "the phase diagram");
    CHECK(a.color == "#2ea8e5");
    CHECK(a.pageLabel == "2");
    CHECK(a.sortIndex == "00001|000000|00109");
    const json pos = json::parse(a.position);
    CHECK(pos["pageIndex"] == 1);
    REQUIRE(pos["rects"].size() == 1);
    auto pdf = samplePdf();
    auto boxes = pdf->highlights(1, {a});
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].kind == HighlightBox::Kind::Image);
    CHECK(boxes[0].rect.w > 300);
    // Sent back unchanged (an edit here keeps Zotero's fields).
    const json sent = annotationJson(a);
    CHECK(sent["annotationType"] == "image");
    CHECK(sent["annotationPosition"] == a.position);
    CHECK(sent["annotationSortIndex"] == a.sortIndex);
    CHECK_FALSE(sent.contains("annotationText"));
}

namespace {

struct SyncRig {
    FakeZotero server;
    std::unique_ptr<Db> db = std::make_unique<Db>(Db::open(":memory:"));
    std::unique_ptr<Library> lib = std::make_unique<Library>(*db);
    std::unique_ptr<WriteGuard> guard;
    std::unique_ptr<ZoteroApi> api;
    std::unique_ptr<SyncEngine> engine;
    SyncRig() {
        Library* l = lib.get();
        guard = std::make_unique<WriteGuard>(server, [l](const std::string& k) { return l->annotation(k).has_value(); },
                                             [l](const std::string& k) { return l->attachment(k).has_value(); });
        api = std::make_unique<ZoteroApi>(*guard, 1, "k3y");
        api->setSleeper([](int) {});
        engine = std::make_unique<SyncEngine>(*api, *lib);
        lib->setAllAnnotationsOffline(true);
        server.put(FakeZotero::item("BOOK0001", "A book"));
        server.put(FakeZotero::attachment("PDFF0001", "BOOK0001", "md5-one"));
        REQUIRE_FALSE(engine->pull([](const SyncProgress&) {}).error);
    }
};

}  // namespace

TEST_CASE("area: one made here is pushed as Zotero's image annotation, and comes back the same") {
    SyncRig rig;
    AnnotationRecord a;
    a.parentKey = "PDFF0001";
    a.type = "image";
    a.color = "#ffd400";
    a.comment = "fig. 2";
    a.pageLabel = "2";
    a.sortIndex = "00001|000000|00092";
    a.position = R"({"pageIndex":1,"rects":[[72.5,300.25,400,500.75]]})";
    const std::string key = rig.lib->createAnnotation(a);
    REQUIRE_FALSE(rig.engine->push().error);
    REQUIRE(rig.server.items.count(key));
    const json d = rig.server.items[key]["data"];
    CHECK(d["itemType"] == "annotation");
    CHECK(d["parentItem"] == "PDFF0001");
    CHECK(d["annotationType"] == "image");
    CHECK(d["annotationColor"] == "#ffd400");
    CHECK(d["annotationComment"] == "fig. 2");
    CHECK(d["annotationPageLabel"] == "2");
    CHECK(d["annotationSortIndex"] == "00001|000000|00092");
    CHECK(d["annotationPosition"] == a.position);
    CHECK_FALSE(d.contains("annotationText"));  // Zotero refuses text on image annotations
    CHECK_FALSE(rig.lib->annotation(key)->dirty);
    // Pulled again (another Kindle): the same area.
    const AnnotationRecord back = parseAnnotation(rig.server.items[key]);
    CHECK(back.type == "image");
    CHECK(back.position == a.position);
    CHECK(back.sortIndex == a.sortIndex);
}

TEST_CASE("area: a conflict on an area is kept like a highlight's") {
    SyncRig rig;
    AnnotationRecord a;
    a.parentKey = "PDFF0001";
    a.type = "image";
    a.color = "#ffd400";
    a.pageLabel = "2";
    a.sortIndex = "00001|000000|00092";
    a.position = R"({"pageIndex":1,"rects":[[72.5,300.25,400,500.75]]})";
    const std::string key = rig.lib->createAnnotation(a);
    REQUIRE_FALSE(rig.engine->push().error);
    // Edited here and on the desktop.
    json data = rig.server.items.at(key)["data"];
    data["annotationComment"] = "desktop";
    rig.server.put(data);
    AnnotationRecord mine = *rig.lib->annotation(key);
    mine.comment = "kindle";
    rig.lib->editAnnotation(mine);
    REQUIRE_FALSE(rig.engine->push().error);  // 412 -> pull -> the Kindle's edit is kept as a conflict
    int conflicts = 0;
    for (const auto& x : rig.lib->annotationsFor("PDFF0001")) conflicts += x.conflict && x.type == "image";
    CHECK(conflicts == 1);
    CHECK(rig.lib->annotation(key)->comment == "desktop");
}

// ---- Notes ---------------------------------------------------------------------------------

TEST_CASE("area: Notes list it as Area · p. N with its comment") {
    addFontDir("assets/fonts-device");
    AnnotationRecord a;
    a.key = "AREA0001";
    a.type = "image";
    a.color = "#ffd400";
    a.pageLabel = "12";
    a.comment = "the phase diagram";
    CHECK(noteRowLabel(a) == "Area · p. 12");
    a.pageLabel.clear();
    CHECK(noteRowLabel(a) == "Area");
    a.type = "highlight";
    CHECK(noteRowLabel(a).empty());
}

// ---- What it looks like --------------------------------------------------------------------

TEST_CASE("area goldens: an area being drawn, and a saved area selected with its bar") {
    Rig rig(DocType::Pdf, 99);  // the fake's page 99: a light gray
    rig.longPressDrag({250, 760}, {900, 1150}, false);
    REQUIRE(rig.waitFor([&] { return rig.reader->drawnArea().has_value(); }));
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/area-drawing.png");
    rig.gesture(GestureType::DragEnd, {900, 1150}, {250, 760});
    REQUIRE(rig.waitFor([&] {
        return rig.editor() && !rig.reader->highlights().empty() &&
               rig.reader->highlights()[0].kind == HighlightBox::Kind::Image;
    }));
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/area-selected.png");
}

TEST_CASE("area goldens: the PDF menus with Mark an area, and the banner while it waits") {
    Rig rig(DocType::Pdf, 99);
    rig.tap({618, 700});
    REQUIRE(rig.reader->menusOpen());
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/area-menus.png");
    rig.tap(ReaderMenus::markAreaRect().center());
    REQUIRE(rig.reader->areaArmed());
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/area-armed.png");
}

TEST_CASE("area: Mark an area's banner and button fit on the KT4 and the Scribe") {
    for (const Metrics* m : {&kKt4, &kScribe, &kPw5}) {
        ScopedMetrics s(*m);
        INFO(m->device);
        const Rect window = m->window();
        const Rect banner = ReaderScreen::areaBannerRect(), cancel = ReaderScreen::areaCancelRect();
        CHECK(window.intersected(banner) == banner);
        CHECK(banner.intersected(cancel) == cancel);
        CHECK(cancel.h >= 80);
        const Rect mark = ReaderMenus::markAreaRect(), top = ReaderMenus::topBarRect();
        CHECK(top.intersected(mark) == mark);
        CHECK(mark.x >= ReaderMenus::backRect().x + ReaderMenus::backRect().w);
        CHECK(mark.x + mark.w <= ReaderMenus::searchRect().x);
        CHECK(mark.h >= 70);
        // The label fits its button.
        CHECK(measureText("Mark an area", {theme::kSans, 8.5, 700}, 10000, 1).width <= mark.w - 20);
    }
}


// ---- Review fixes --------------------------------------------------------------------------

TEST_CASE("area: Mark an area and a half-drawn area end on hide, background, a jump or a turn") {
    Rig rig;
    auto arm = [&] {
        rig.tap({618, 700});
        rig.tap(ReaderMenus::markAreaRect().center());
        REQUIRE(rig.reader->areaArmed());
    };
    arm();
    rig.reader->onHide(rig.nav);
    CHECK_FALSE(rig.reader->areaArmed());
    CHECK_FALSE(rig.reader->dragsAtOnce());
    arm();
    rig.reader->onBackground(rig.nav);
    CHECK_FALSE(rig.reader->areaArmed());
    arm();
    rig.reader->turnTo(5);
    CHECK_FALSE(rig.reader->areaArmed());
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 5; }));
    // Half drawn with a finger, then the app goes to the background: the rest of the drag saves nothing.
    rig.longPressDrag(kBlank, {500, 950}, false);
    REQUIRE(rig.waitFor([&] { return rig.reader->drawnArea().has_value(); }));
    rig.reader->onBackground(rig.nav);
    CHECK_FALSE(rig.reader->drawnArea());
    rig.gesture(GestureType::DragMove, kBlankFar, kBlank);
    rig.gesture(GestureType::DragEnd, kBlankFar, kBlank);
    // A long press still waiting to learn if it was on text, then hidden: no area later either.
    rig.gesture(GestureType::LongPress, kBlank, kBlank);
    rig.reader->onHide(rig.nav);
    rig.gesture(GestureType::DragEnd, kBlankFar, kBlank);
    // Half drawn with the pen (Mark an area), then a jump: lifting the pen saves nothing, draws no ink.
    arm();
    CHECK(rig.reader->onPen(pen(PenEvent::Type::Down, kBlank), rig.nav));
    rig.reader->onPen(pen(PenEvent::Type::Move, kBlankFar), rig.nav);
    rig.reader->jumpTo(DocPosition{"pos:0"});
    rig.reader->onPen(pen(PenEvent::Type::Up, kBlankFar), rig.nav);
    rig.settle(200);
    CHECK(rig.areas().empty());
    CHECK(rig.reader->pendingInkStrokes() == 0);
    CHECK_FALSE(rig.editor());
}

TEST_CASE("area: an area is kept to the page as drawn, not the margins around it") {
    Rig rig(DocType::Pdf, 0, {100, 100, 1000, 1200});
    rig.longPressDrag(kBlank, {1200, 1450});
    REQUIRE(rig.waitFor([&] { return rig.areas().size() == 1; }));
    CHECK(json::parse(rig.areas()[0].position)["rect"] == json::array({300, 800, 800, 500}));
    // A real PDF page: its box sits inside the page area, and an area past its edge is cut to the page.
    auto pdf = samplePdf();
    const Rect box = pdf->pageBox(1);
    CHECK(box.w > 900);
    CHECK(box.w < 1236);  // a letter page fitted by its height: white beside it
    CHECK(box.h >= 1480);
    auto placed = pdf->areaPosition(1, {box.x - 100, 400, box.w + 200, 300});
    REQUIRE(placed);
    const auto r = json::parse(placed->position)["rects"][0];
    CHECK(r[0].get<double>() >= 0);
    CHECK(r[2].get<double>() <= 612.01);
}

namespace {

// The whole app, with the reader on a fake PDF: the pen goes through App::handlePen.
struct AppRig {
    FakeDisplay display;
    FakeSync sync;
    NoKeyboard keyboard;
    Db db{Db::open(":memory:")};
    Library lib{db};
    int64_t now = 1000;
    std::unique_ptr<App> app;
    ReaderScreen* reader = nullptr;
    AppRig() {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        for (auto& a : lib.annotationsFor("EP000003")) lib.removeObject("annotation", a.key);
        app = std::make_unique<App>(display, lib, sync, keyboard, [this] { return now; });
        auto r = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book.pdf", DocType::Pdf, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
        }, 6);
        reader = r.get();
        app->setRoot(0, std::move(r));
        app->start();
        REQUIRE(run([&] { return reader->ready(); }));
    }
    bool run(const std::function<bool()>& done, int steps = 200) {
        for (int i = 0; i < steps; ++i) {
            app->step(0);
            if (done && done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            now += 10;
        }
        return done ? done() : true;
    }
    // A side-button stroke held still for `holdMs`, then dragged to `to` and lifted.
    void buttonStroke(Point from, Point to, int holdMs) {
        display.pen(PenEvent::Type::Near, from, now, false, true);
        display.pen(PenEvent::Type::Down, from, now, false, true);
        run(nullptr, 2);
        now += holdMs;
        run(nullptr, 2);
        display.pen(PenEvent::Type::Move, {(from.x + to.x) / 2, (from.y + to.y) / 2}, now, false, true);
        display.pen(PenEvent::Type::Move, to, now + 10, false, true);
        display.pen(PenEvent::Type::Up, to, now + 20, false, true);
        display.pen(PenEvent::Type::Leave, to, now + 30, false, true);
        run(nullptr, 3);
    }
    std::vector<AnnotationRecord> all() { return lib.annotationsFor("EP000003"); }
};

}  // namespace

TEST_CASE("area: the pen's side button is a finger: a long press and drag on text selects, on blank draws an area") {
    AppRig rig;
    rig.buttonStroke(kOnText, {650, 330}, 700);
    REQUIRE(rig.run([&] { return rig.reader->annotationBar() != nullptr; }));
    CHECK(rig.reader->selection());
    CHECK(rig.all().empty());
    rig.reader->onGesture(tapAt({618, 1400}), *rig.app);  // closes the selection
    REQUIRE_FALSE(rig.reader->selection());
    rig.buttonStroke(kBlank, kBlankFar, 700);
    REQUIRE(rig.run([&] { return rig.all().size() == 1; }));
    CHECK(rig.all()[0].type == "image");
    CHECK(json::parse(rig.all()[0].position)["rect"] == json::array({300, 800, 400, 300}));
    // A quick side-button drag (no long press) is a swipe, as with a finger: no area, no ink.
    rig.reader->onGesture(tapAt({618, 1400}), *rig.app);
    rig.buttonStroke({300, 1200}, {700, 1210}, 0);
    rig.run(nullptr, 20);
    CHECK(rig.all().size() == 1);
    CHECK(rig.reader->pendingInkStrokes() == 0);
}

TEST_CASE("area: while an area is dragged only its thin edges are refreshed") {
    AppRig rig;
    rig.display.touch(TouchEvent::Type::Down, kBlank, rig.now);
    rig.run(nullptr, 2);
    rig.now += 700;
    rig.run(nullptr, 2);
    rig.display.touch(TouchEvent::Type::Move, {500, 950}, rig.now);
    REQUIRE(rig.run([&] { return rig.reader->drawnArea().has_value(); }));
    rig.run(nullptr, 3);
    rig.display.presents.clear();
    rig.display.touch(TouchEvent::Type::Move, kBlankFar, rig.now);
    rig.run(nullptr, 3);
    int fast = 0;
    for (const auto& p : rig.display.presents)
        if (p.waveform == Waveform::Fast) {
            ++fast;
            CHECK((p.rect.w < 40 || p.rect.h < 40));
        }
    CHECK(fast >= 4);
    rig.display.touch(TouchEvent::Type::Up, kBlankFar, rig.now);
    REQUIRE(rig.run([&] { return rig.all().size() == 1; }));
}
