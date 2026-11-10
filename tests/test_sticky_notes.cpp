// Sticky notes (Zotero `note` annotations) on PDF pages: the menus' "Add note", a tap where it
// goes, its text in the note box; a tap near one opens its pop-up (Edit, Delete, Close). Synced as
// Zotero's note annotations.

#include <doctest/doctest.h>

#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_zotero.h"
#include "fixture_library.h"
#include "golden.h"
#include "net/write_guard.h"
#include "screens/annotation_bar.h"
#include "screens/note_popup.h"
#include "screens/reader.h"
#include "screens/reader_menus.h"
#include "store/db.h"
#include "sync/sync_engine.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "zotero/api.h"
#include "zotero/parse.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    ItemRow row;
    std::unique_ptr<ReaderScreen> reader;

    explicit Rig(DocType type = DocType::Pdf, int page = 0) {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        for (auto& a : lib.annotationsFor("EP000003")) lib.removeObject("annotation", a.key);
        row = *lib.rowFor("IT000003");
        reader = std::make_unique<ReaderScreen>(row, type == DocType::Pdf ? "book.pdf" : "book.epub", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
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
    void tap(Point p) { reader->onGesture(tapAt(p), nav); }
    void type(const std::string& s) { reader->onKey(KeyEvent{KeyEvent::Code::Text, s}, nav); }
    void enter() { reader->onKey(KeyEvent{KeyEvent::Code::Return, ""}, nav); }
    void arm() {
        tap({618, 700});
        REQUIRE(reader->menusOpen());
        tap(ReaderMenus::addNoteRect().center());
        REQUIRE(reader->noteArmed());
    }
    std::vector<AnnotationRecord> notes() {
        std::vector<AnnotationRecord> out;
        for (auto& a : lib.annotationsFor(row.attachmentKey))
            if (a.type == "note") out.push_back(a);
        return out;
    }
    // A note saved on page 0 with its icon's corner at `at` (the fake draws it 40 px square there).
    void putNote(const std::string& key, Point at, const std::string& text) {
        AnnotationRecord a;
        a.key = key;
        a.parentKey = row.attachmentKey;
        a.type = "note";
        a.color = "#ffd400";
        a.comment = text;
        a.pageLabel = "0";
        a.version = 3;
        a.position = json{{"fake", 0}, {"note", {at.x, at.y}}}.dump();
        lib.upsertAnnotation(a);
        SyncEvent ready;
        ready.kind = SyncEvent::Kind::AnnotationsReady;
        ready.attachmentKey = row.attachmentKey;
        reader->onSyncEvent(ready, nav);
        REQUIRE(waitFor([&] {
            for (const auto& b : reader->highlights())
                if (b.annotationKey == key) return true;
            return false;
        }));
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

// ---- Making one ----------------------------------------------------------------------------

TEST_CASE("sticky note: Add note arms the reader; Cancel ends it; other taps wait") {
    Rig rig;
    rig.arm();
    CHECK_FALSE(rig.reader->menusOpen());
    CHECK_FALSE(rig.reader->areaArmed());
    CHECK_FALSE(rig.reader->dragsAtOnce());
    // A swipe or a long press does nothing meanwhile; Cancel ends it.
    rig.reader->onGesture(Gesture{GestureType::Swipe, {300, 700}, {900, 700}, Direction::Left}, rig.nav);
    rig.reader->onGesture(Gesture{GestureType::LongPress, {300, 800}, {300, 800}}, rig.nav);
    rig.settle();
    CHECK(rig.reader->page() == 0);
    CHECK_FALSE(rig.reader->selection());
    CHECK(rig.reader->noteArmed());
    rig.tap(ReaderScreen::areaCancelRect().center());
    CHECK_FALSE(rig.reader->noteArmed());
    CHECK_FALSE(rig.reader->noteOpen());
    rig.tap({618, 700});
    CHECK(rig.reader->menusOpen());
}

TEST_CASE("sticky note: a tap opens the note box; Return with text saves a note there") {
    Rig rig;
    rig.arm();
    rig.tap({420, 900});
    CHECK_FALSE(rig.reader->noteArmed());
    CHECK(rig.reader->placingNote());
    CHECK(rig.reader->noteOpen());
    CHECK(rig.nav.keyboardFake.shown);
    CHECK(rig.reader->wantsKeyboard());
    rig.type("check eq. 4");
    rig.enter();
    CHECK_FALSE(rig.reader->noteOpen());
    CHECK_FALSE(rig.nav.keyboardFake.shown);
    REQUIRE(rig.waitFor([&] { return rig.notes().size() == 1; }));
    const AnnotationRecord n = rig.notes()[0];
    CHECK(n.comment == "check eq. 4");
    CHECK(n.text.empty());
    CHECK(n.color == "#ffd400");  // the colour used last
    CHECK(json::parse(n.position)["note"] == json::array({420, 900}));
    CHECK(n.sortIndex == "00000|00000900");  // the engine's, as an area's
    CHECK(n.pageLabel == "0");
    CHECK(rig.nav.syncFake.pushes > 0);
    // Drawn by the page as a note, where it was tapped.
    REQUIRE(rig.waitFor([&] { return !rig.reader->highlights().empty(); }));
    CHECK(rig.reader->highlights()[0].kind == HighlightBox::Kind::Note);
    CHECK(rig.reader->highlights()[0].rect.x == 420);
}

TEST_CASE("sticky note: empty text, Escape or a tap elsewhere creates nothing") {
    Rig rig;
    rig.arm();
    rig.tap({420, 900});
    rig.enter();  // empty
    rig.arm();
    rig.tap({420, 900});
    rig.type("   ");
    rig.enter();  // only spaces
    rig.arm();
    rig.tap({420, 900});
    rig.type("gone");
    rig.reader->onKey(KeyEvent{KeyEvent::Code::Escape, ""}, rig.nav);
    CHECK_FALSE(rig.reader->placingNote());
    rig.arm();
    rig.tap({420, 900});
    rig.type("gone too");
    rig.tap({618, 100});  // outside the note box
    CHECK_FALSE(rig.reader->noteOpen());
    CHECK_FALSE(rig.nav.keyboardFake.shown);
    rig.settle(200);
    CHECK(rig.notes().empty());
    CHECK_FALSE(rig.reader->placingNote());
}

TEST_CASE("sticky note: Add note ends on hide, background, a turn or a jump") {
    Rig rig;
    rig.arm();
    rig.reader->onHide(rig.nav);
    CHECK_FALSE(rig.reader->noteArmed());
    rig.arm();
    rig.reader->onBackground(rig.nav);
    CHECK_FALSE(rig.reader->noteArmed());
    rig.arm();
    rig.reader->turnTo(5);
    CHECK_FALSE(rig.reader->noteArmed());
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 5; }));
    rig.arm();
    rig.reader->jumpTo(DocPosition{"pos:0"});
    CHECK_FALSE(rig.reader->noteArmed());
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 0; }));
    rig.arm();
    rig.reader->goToPage(3);
    CHECK_FALSE(rig.reader->noteArmed());
    // Mark area after Add note: only one waits.
    rig.settle();
    rig.arm();
    rig.tap({618, 100});  // a tap places the note box: give up on it
    rig.tap({618, 100});
    rig.tap({618, 700});
    rig.tap(ReaderMenus::markAreaRect().center());
    CHECK(rig.reader->areaArmed());
    CHECK_FALSE(rig.reader->noteArmed());
}

TEST_CASE("sticky note: not offered in an EPUB") {
    Rig rig(DocType::Epub);
    rig.tap({618, 700});
    REQUIRE(rig.reader->menusOpen());
    CHECK(rig.reader->menus()->buttonCount() == 5);
    rig.reader->addNote(rig.nav);
    CHECK_FALSE(rig.reader->noteArmed());
    FakeEngine plain;
    DocumentEngine& base = plain;
    CHECK_FALSE(base.DocumentEngine::notePosition(0, {10, 10}));
}

// ---- Its place in PDF points -----------------------------------------------------------------

TEST_CASE("sticky note: a PDF note is a 22 pt square, its top-left corner at the tap, kept on the page") {
    auto pdf = samplePdf();
    const Rect box = pdf->pageBox(1);
    const Point at{box.x + 300, 500};
    auto placed = pdf->notePosition(1, at);
    REQUIRE(placed);
    const json pos = json::parse(placed->position);
    CHECK(pos.size() == 2);  // pageIndex and rects only
    CHECK(pos["pageIndex"] == 1);
    REQUIRE(pos["rects"].size() == 1);
    const auto r = pos["rects"][0];
    CHECK(r[2].get<double>() - r[0].get<double>() == doctest::Approx(22).epsilon(0.001));
    CHECK(r[3].get<double>() - r[1].get<double>() == doctest::Approx(22).epsilon(0.001));
    CHECK(placed->sortIndex.rfind("00001|", 0) == 0);
    CHECK(placed->sortIndex.size() == 18);
    CHECK(placed->pageLabel == "2");
    // Drawn back with its top-left corner where it was tapped.
    AnnotationRecord a;
    a.key = "NOTERT01";
    a.type = "note";
    a.color = "#ffd400";
    a.position = placed->position;
    auto boxes = pdf->highlights(1, {a});
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].kind == HighlightBox::Kind::Note);
    CHECK(std::abs(boxes[0].rect.x - at.x) <= 1);
    CHECK(std::abs(boxes[0].rect.y - at.y) <= 1);
    // Tapped at the page's bottom-right corner, and past its left edge: moved inside the page box
    // (a letter page, 612 × 792 pt).
    for (Point edge : {Point{box.x + box.w - 2, box.y + box.h - 2}, Point{box.x - 40, box.y + 3}}) {
        auto e = pdf->notePosition(1, edge);
        REQUIRE(e);
        const auto q = json::parse(e->position)["rects"][0];
        CHECK(q[0].get<double>() >= 0);
        CHECK(q[1].get<double>() >= 0);
        CHECK(q[2].get<double>() <= 612.01);
        CHECK(q[3].get<double>() <= 792.01);
        CHECK(q[2].get<double>() - q[0].get<double>() == doctest::Approx(22).epsilon(0.001));
        CHECK(q[3].get<double>() - q[1].get<double>() == doctest::Approx(22).epsilon(0.001));
    }
    auto corner = json::parse(pdf->notePosition(1, {box.x + box.w - 2, box.y + box.h - 2})->position)["rects"][0];
    CHECK(corner[2].get<double>() == doctest::Approx(612).epsilon(0.001));
    CHECK(corner[1].get<double>() == doctest::Approx(0).epsilon(0.001));  // its bottom on the page's
}

// ---- An existing note: its pop-up -------------------------------------------------------------

TEST_CASE("sticky note: a tap within ~9 mm of a note opens its pop-up, before a highlight under it") {
    Rig rig;
    // A highlight at {100, 200, 600, 50} (the fake's place for it) and a note on it.
    AnnotationRecord h;
    h.key = "HIGH0001";
    h.parentKey = rig.row.attachmentKey;
    h.type = "highlight";
    h.text = "words";
    h.color = "#ffd400";
    h.pageLabel = "0";
    rig.lib.upsertAnnotation(h);
    rig.putNote("NOTE0001", {300, 210}, "See the appendix.");
    rig.tap({320, 230});  // on the icon, over the highlight
    REQUIRE(rig.reader->notePopup());
    CHECK(rig.reader->notePopup()->key() == "NOTE0001");
    CHECK(rig.reader->notePopup()->title() == "Note · p. 0");
    CHECK_FALSE(rig.editor());
    rig.tap({618, 1400});  // a tap elsewhere closes it, and is only that
    CHECK_FALSE(rig.reader->notePopup());
    CHECK_FALSE(rig.reader->menusOpen());
    CHECK(rig.reader->page() == 0);
    // Within ~9 mm of the 40 px icon, all round; further, the page's (the highlight's editor here).
    rig.tap({300 + 40 + ReaderScreen::kNoteReachPx - 2, 230});
    REQUIRE(rig.reader->notePopup());
    rig.tap(rig.reader->notePopup()->buttonRect(2).center());  // Close
    CHECK_FALSE(rig.reader->notePopup());
    rig.tap({300 + 40 + ReaderScreen::kNoteReachPx + 20, 225});
    CHECK_FALSE(rig.reader->notePopup());
    REQUIRE(rig.editor());
    CHECK(rig.editor()->key() == "HIGH0001");
    CHECK(ReaderScreen::kNoteReachPx * 25.4 / 300 >= 9);
}

TEST_CASE("sticky note: the nearest of two notes wins; a long one pages with swipes") {
    Rig rig;
    std::string longText;
    for (int i = 0; i < 80; ++i) longText += "A sentence that goes on for a while, number " + std::to_string(i) + ". ";
    rig.putNote("NOTE0001", {300, 900}, "near");
    rig.putNote("NOTE0002", {500, 900}, longText);
    rig.tap({470, 920});
    REQUIRE(rig.reader->notePopup());
    CHECK(rig.reader->notePopup()->key() == "NOTE0002");
    const NotePopup* p = rig.reader->notePopup();
    CHECK(p->pages() > 1);
    CHECK(ReaderScreen::pageArea().intersected(p->rect()) == p->rect());
    rig.reader->onGesture(Gesture{GestureType::Swipe, {300, p->rect().y + 200}, {900, p->rect().y + 200}, Direction::Left}, rig.nav);
    REQUIRE(rig.reader->notePopup());
    CHECK(rig.reader->notePopup()->page() == 1);
    CHECK(rig.reader->page() == 0);  // the page didn't turn
    rig.reader->onGesture(Gesture{GestureType::Swipe, {900, 500}, {300, 500}, Direction::Right}, rig.nav);
    CHECK(rig.reader->notePopup()->page() == 0);
    // A turn closes it.
    rig.reader->turnTo(4);
    CHECK_FALSE(rig.reader->notePopup());
}

TEST_CASE("sticky note: Edit opens the note box on its text and saves it") {
    Rig rig;
    rig.putNote("NOTE0001", {300, 900}, "first");
    rig.tap({310, 910});
    REQUIRE(rig.reader->notePopup());
    rig.tap(rig.reader->notePopup()->buttonRect(0).center());  // Edit
    CHECK_FALSE(rig.reader->notePopup());
    REQUIRE(rig.editor());
    CHECK(rig.editor()->commenting());
    CHECK(rig.nav.keyboardFake.shown);
    rig.type(" and second");
    rig.enter();
    CHECK_FALSE(rig.editor());
    const auto n = rig.lib.annotation("NOTE0001");
    REQUIRE(n);
    CHECK(n->comment == "first and second");
    CHECK(n->dirty);
    CHECK(n->position == json{{"fake", 0}, {"note", {300, 900}}}.dump());  // only the comment changed
}

TEST_CASE("sticky note: Delete asks, then deletes it through the usual deletion; Cancel keeps it") {
    Rig rig;
    rig.putNote("NOTE0001", {300, 900}, "doomed");
    rig.tap({310, 910});
    rig.tap(rig.reader->notePopup()->buttonRect(1).center());  // Delete
    REQUIRE(rig.editor());
    REQUIRE(rig.editor()->confirming());
    CHECK(rig.editor()->question() == "Delete this note?");
    rig.tap(rig.editor()->confirmRect(1).center());  // Cancel: closed, kept
    CHECK_FALSE(rig.editor());
    CHECK(rig.notes().size() == 1);
    rig.tap({310, 910});
    rig.tap(rig.reader->notePopup()->buttonRect(1).center());
    rig.tap(rig.editor()->confirmRect(0).center());  // Delete
    CHECK_FALSE(rig.editor());
    CHECK(rig.notes().empty());
    REQUIRE(rig.lib.pendingDeletions().size() == 1);
    CHECK(rig.lib.pendingDeletions()[0].key == "NOTE0001");
    REQUIRE(rig.waitFor([&] { return rig.reader->highlights().empty(); }));
}

// ---- Lists ---------------------------------------------------------------------------------

TEST_CASE("sticky note: Notes and book notes list it as Note · p. N with its text") {
    addFontDir("assets/fonts-device");
    AnnotationRecord a = parseAnnotation(fixture("annotation_note.json"));
    CHECK(noteRowLabel(a) == "Note · p. 2");
    a.pageLabel.clear();
    CHECK(noteRowLabel(a) == "Note");
    a.pageLabel = "2";
    Canvas c(1236, kNoteRowH);
    drawNoteRow(c.cr(), {0, 0, 1236, kNoteRowH}, a);  // the label line, then the text (no crash, ink drawn)
    std::vector<uint8_t> g = grayOf(c);
    size_t dark = 0;
    for (uint8_t v : g) dark += v < 128;
    CHECK(dark > 500);
}

// ---- Sync ----------------------------------------------------------------------------------

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

TEST_CASE("sticky note: one made in Zotero parses, and shows as a note on its page") {
    const AnnotationRecord a = parseAnnotation(fixture("annotation_note.json"));
    CHECK(a.type == "note");
    CHECK(a.text.empty());
    CHECK(a.comment == "Compare with the 1998 survey — the sign flips here.");
    CHECK(a.color == "#ff6666");
    CHECK(a.pageLabel == "2");
    auto pdf = samplePdf();
    auto boxes = pdf->highlights(1, {a});
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].kind == HighlightBox::Kind::Note);
    CHECK(boxes[0].rect.w > 20);
    const json sent = annotationJson(a);  // sent back as it came
    CHECK(sent["annotationType"] == "note");
    CHECK(sent["annotationPosition"] == a.position);
    CHECK_FALSE(sent.contains("annotationText"));
}

TEST_CASE("sticky note: one made here is pushed as Zotero's note annotation, and comes back the same") {
    SyncRig rig;
    AnnotationRecord a;
    a.parentKey = "PDFF0001";
    a.type = "note";
    a.color = "#ffd400";
    a.comment = "ask about this";
    a.pageLabel = "2";
    a.sortIndex = "00001|000000|00092";
    a.position = R"({"pageIndex":1,"rects":[[72.5,300.25,94.5,322.25]]})";
    const std::string key = rig.lib->createAnnotation(a);
    REQUIRE_FALSE(rig.engine->push().error);
    REQUIRE(rig.server.items.count(key));
    const json d = rig.server.items[key]["data"];
    CHECK(d["annotationType"] == "note");
    CHECK(d["annotationComment"] == "ask about this");
    CHECK(d["annotationColor"] == "#ffd400");
    CHECK(d["annotationPosition"] == a.position);
    CHECK(d["annotationSortIndex"] == a.sortIndex);
    CHECK_FALSE(d.contains("annotationText"));
    CHECK_FALSE(rig.lib->annotation(key)->dirty);
    const AnnotationRecord back = parseAnnotation(rig.server.items[key]);
    CHECK(back.type == "note");
    CHECK(back.comment == a.comment);
    CHECK(back.position == a.position);
    // Edited on both sides: the Kindle's is kept as a conflict, as for highlights.
    json data = rig.server.items.at(key)["data"];
    data["annotationComment"] = "desktop";
    rig.server.put(data);
    AnnotationRecord mine = *rig.lib->annotation(key);
    mine.comment = "kindle";
    rig.lib->editAnnotation(mine);
    REQUIRE_FALSE(rig.engine->push().error);
    int conflicts = 0;
    for (const auto& x : rig.lib->annotationsFor("PDFF0001")) conflicts += x.conflict && x.type == "note";
    CHECK(conflicts == 1);
    CHECK(rig.lib->annotation(key)->comment == "desktop");
}

// ---- Fit and looks -------------------------------------------------------------------------

TEST_CASE("sticky note: the seven cells, Add note's banner and the pop-up fit on the PW5, KT4 and Scribe") {
    addFontDir("assets/fonts-device");
    for (const Metrics* m : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics s(*m);
        INFO(m->device);
        const Rect panel = ReaderMenus::panelRect();
        CHECK(ReaderMenus::addNoteRect() == ReaderMenus::buttonRect(6, 7));
        for (int i = 0; i < 7; ++i) {
            const Rect c = ReaderMenus::buttonRect(i, 7);
            CHECK(panel.intersected(c) == c);
            CHECK(c.w >= 106);  // ~9 mm
            CHECK(c.h >= 106);
            CHECK(c.w >= kIconSize + 20);
            if (i) CHECK(c.x > ReaderMenus::buttonRect(i - 1, 7).x + ReaderMenus::buttonRect(i - 1, 7).w);
        }
        CHECK(48 + kIconSize / 2 <= 84);  // the icon's bottom (centre 48) above the label's top (84)
        const int room = ReaderMenus::buttonRect(0, 7).w - 8;
        for (const char* l : {"Contents", "Notes (12)", "Notes (128)", "Display", "Light", "Syncing…", "Mark area", "Add note"}) {
            INFO(l);
            const TextStyle st = ReaderMenus::labelStyle(l, room);
            CHECK(st.size_pt >= 5.5);
            CHECK(measureText(l, st, 10000, 1).width <= room);
        }
        const Rect banner = ReaderScreen::areaBannerRect(), cancel = ReaderScreen::areaCancelRect();
        CHECK(m->window().intersected(banner) == banner);
        CHECK(measureText("Tap where the note goes.", {theme::kSans, 8, 500}, 10000, 1).width <= cancel.x - banner.x - 60);
        // The pop-up of a note in a corner stays on the page area; its buttons ~9 mm or more.
        AnnotationRecord a;
        a.type = "note";
        a.pageLabel = "3";
        a.comment = "a note";
        const Rect area = ReaderScreen::pageArea();
        for (Rect icon : {Rect{area.x + 10, area.y + 10, 40, 40}, Rect{area.x + area.w - 50, area.y + area.h - 50, 40, 40}}) {
            NotePopup p(a, icon, area);
            CHECK(area.intersected(p.rect()) == p.rect());
            CHECK(p.rect().intersected(icon).empty());
            for (int i = 0; i < 3; ++i) {
                CHECK(p.buttonRect(i).w >= 106);
                CHECK(p.buttonRect(i).h >= 80);
            }
        }
    }
}

TEST_CASE("sticky note goldens: the PDF menus with Add note, its banner, and a note's pop-up") {
    Rig rig(DocType::Pdf, 99);  // the fake's page 99: a light gray
    rig.tap({618, 700});
    REQUIRE(rig.reader->menusOpen());
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/note-menus.png");
    rig.tap(ReaderMenus::addNoteRect().center());
    REQUIRE(rig.reader->noteArmed());
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/note-armed.png");
    rig.tap(ReaderScreen::areaCancelRect().center());
    AnnotationRecord a;
    a.key = "NOTEGLD1";
    a.parentKey = rig.row.attachmentKey;
    a.type = "note";
    a.color = "#ffd400";
    a.comment = "Compare with the 1998 survey: the sign of the effect flips once firm size is controlled for. Ask "
                "whether the sample overlaps.";
    a.pageLabel = "99";
    a.position = json{{"fake", 99}, {"note", {820, 520}}}.dump();
    rig.lib.upsertAnnotation(a);
    SyncEvent ready;
    ready.kind = SyncEvent::Kind::AnnotationsReady;
    ready.attachmentKey = rig.row.attachmentKey;
    rig.reader->onSyncEvent(ready, rig.nav);
    REQUIRE(rig.waitFor([&] { return !rig.reader->highlights().empty(); }));
    rig.tap({840, 540});
    REQUIRE(rig.reader->notePopup());
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/note-popup.png");
}
