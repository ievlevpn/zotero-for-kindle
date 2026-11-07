// Plan 5 Task 8: tapping a highlight in the reader (or long-pressing it in Book Notes) to
// recolour it, edit its comment or delete it.

#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/annotation_bar.h"
#include "screens/book_notes.h"
#include "screens/reader.h"
#include "store/db.h"
#include "ui/text.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

constexpr Point kOnBox{400, 225};  // FakeEngine draws a page's annotations at {100, 200, 600, 50}

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;

    explicit Rig(int64_t version = 5) {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        for (auto& a : lib.annotationsFor("EP000003")) lib.removeObject("annotation", a.key);
        AnnotationRecord a;
        a.key = "EDITME01";
        a.parentKey = "EP000003";
        a.type = "highlight";
        a.text = "a passage";
        a.color = "#ffd400";
        a.pageLabel = "0";  // the fake engine's page
        a.version = version;
        lib.upsertAnnotation(a);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "book.epub", DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(waitFor([&] { return reader->ready() && !reader->highlights().empty(); }));
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    void tap(Point p) { reader->onGesture(tapAt(p), nav); }
    AnnotationEditor* editor() { return reader->annotationEditor(); }
};

}  // namespace

TEST_CASE("annotation edit: a tap on a highlight opens the edit bar for it, not the menus") {
    Rig rig;
    rig.nav.damages.clear();
    rig.tap(kOnBox);
    REQUIRE(rig.editor());
    CHECK(rig.editor()->key() == "EDITME01");
    CHECK_FALSE(rig.reader->menusOpen());
    CHECK(rig.editor()->bar().mode() == AnnotationBar::Mode::Edit);
    CHECK(rig.editor()->bar().colour() == "#ffd400");
    bool ui = false;
    for (const auto& d : rig.nav.damages) ui |= d.waveform == Waveform::Ui && d.rect == rig.editor()->rect();
    CHECK(ui);
    // A tap in the middle away from highlights still opens the menus.
    rig.tap({600, 900});
    CHECK_FALSE(rig.editor());
    rig.tap({600, 900});
    CHECK(rig.reader->menusOpen());
}

TEST_CASE("annotation edit: a colour dot recolours the highlight at once and marks it to be sent") {
    Rig rig;
    rig.tap(kOnBox);
    REQUIRE(rig.editor());
    rig.tap(rig.editor()->bar().dotRect(3).center());  // Blue
    auto a = rig.lib.annotation("EDITME01");
    CHECK(a->color == "#2ea8e5");
    CHECK(a->dirty);
    CHECK(a->version == 5);
    REQUIRE(rig.editor());  // stays open for more changes
    CHECK(rig.editor()->bar().colour() == "#2ea8e5");
    REQUIRE(rig.waitFor([&] { return !rig.reader->highlights().empty() && rig.reader->highlights()[0].shade == shadeOf("#2ea8e5"); }));
}

TEST_CASE("annotation edit: Comment edits the comment with the keyboard") {
    Rig rig;
    rig.tap(kOnBox);
    rig.tap(rig.editor()->bar().buttonRect(0).center());  // Comment
    CHECK(rig.reader->wantsKeyboard());
    rig.reader->onKey(KeyEvent{KeyEvent::Code::Text, "see chapter 3"}, rig.nav);
    rig.reader->onKey(KeyEvent{KeyEvent::Code::Return, ""}, rig.nav);
    auto a = rig.lib.annotation("EDITME01");
    CHECK(a->comment == "see chapter 3");
    CHECK(a->dirty);
    CHECK_FALSE(rig.editor());
    CHECK_FALSE(rig.reader->wantsKeyboard());
}

TEST_CASE("annotation edit: Delete asks first; Cancel keeps the highlight") {
    Rig rig;
    rig.tap(kOnBox);
    rig.tap(rig.editor()->bar().buttonRect(1).center());  // Delete
    REQUIRE(rig.editor());
    CHECK(rig.editor()->confirming());
    rig.tap(rig.editor()->confirmRect(1).center());  // Cancel
    REQUIRE(rig.editor());
    CHECK_FALSE(rig.editor()->confirming());
    CHECK(rig.lib.annotation("EDITME01"));
    CHECK_FALSE(rig.lib.annotation("EDITME01")->dirty);
}

TEST_CASE("annotation edit: deleting a synced highlight leaves a tombstone to send and clears the page") {
    Rig rig;
    rig.tap(kOnBox);
    rig.tap(rig.editor()->bar().buttonRect(1).center());
    rig.nav.damages.clear();
    rig.tap(rig.editor()->confirmRect(0).center());  // Delete
    CHECK_FALSE(rig.editor());
    REQUIRE(rig.lib.pendingDeletions().size() == 1);
    CHECK(rig.lib.pendingDeletions()[0].key == "EDITME01");
    CHECK(rig.lib.annotationsFor("EP000003").empty());
    bool quality = false;
    for (const auto& d : rig.nav.damages) quality |= d.waveform == Waveform::Quality && d.rect == ReaderScreen::pageArea();
    CHECK(quality);
    REQUIRE(rig.waitFor([&] { return rig.reader->highlights().empty(); }));
}

TEST_CASE("annotation edit: deleting a highlight that was never sent removes it") {
    Rig rig(0);
    rig.tap(kOnBox);
    rig.tap(rig.editor()->bar().buttonRect(1).center());
    rig.tap(rig.editor()->confirmRect(0).center());
    CHECK_FALSE(rig.lib.annotation("EDITME01"));
    CHECK(rig.lib.pendingDeletions().empty());
}

TEST_CASE("annotation edit: page-edge taps still turn pages over a highlight") {
    Rig rig;
    rig.tap({1150, 225});
    CHECK_FALSE(rig.editor());
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 1; }));
}

TEST_CASE("annotation edit: Book Notes rows offer the same actions on long-press") {
    Rig rig;
    BookNotesScreen notes(*rig.lib.rowFor("IT000003"), nullptr);
    rig.nav.visible = &notes;
    notes.onShow(rig.nav);
    REQUIRE(notes.rowRects().size() == 1);
    const Point row = notes.rowRects()[0].center();
    notes.onGesture(Gesture{GestureType::LongPress, row, row}, rig.nav);
    REQUIRE(notes.annotationEditor());
    notes.onGesture(tapAt(notes.annotationEditor()->bar().dotRect(2).center()), rig.nav);  // Green
    CHECK(rig.lib.annotation("EDITME01")->color == "#5fb236");
    notes.onGesture(tapAt(notes.annotationEditor()->bar().buttonRect(1).center()), rig.nav);
    notes.onGesture(tapAt(notes.annotationEditor()->confirmRect(0).center()), rig.nav);
    CHECK_FALSE(notes.annotationEditor());
    CHECK(notes.rowRects().empty());
    CHECK(rig.lib.pendingDeletions().size() == 1);
}

TEST_CASE("annotation edit bar matches the reviewed golden") {
    Rig rig;
    rig.reader->turnTo(99);
    REQUIRE(rig.waitFor([&] { return rig.reader->page() == 99; }));
    AnnotationRecord a = *rig.lib.annotation("EDITME01");
    a.pageLabel = "99";
    rig.lib.upsertAnnotation(a);
    SyncEvent ready;
    ready.kind = SyncEvent::Kind::AnnotationsReady;
    ready.attachmentKey = "EP000003";
    rig.reader->onSyncEvent(ready, rig.nav);
    REQUIRE(rig.waitFor([&] { return !rig.reader->highlights().empty(); }));
    rig.tap(kOnBox);
    REQUIRE(rig.editor());
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    checkGolden(c, "tests/golden/annotation-edit.png");
}
