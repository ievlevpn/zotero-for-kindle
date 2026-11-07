#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/book.h"
#include "screens/library.h"
#include "screens/notes.h"
#include "screens/open_flow.h"
#include "store/db.h"
#include "ui/text.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kWall = 1'762'500'000'000;

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    OpenFlow flow{[] { return kWall; }};
    std::unique_ptr<LibraryScreen> screen;
    Rig() {
        fillFixtureLibrary(lib, 40);
        screen = std::make_unique<LibraryScreen>([this](const ItemRow& r) { flow.open(r, nav); },
                                                 [this](const std::string& k) { return flow.statusFor(k); });
        nav.visible = screen.get();
        screen->onShow(nav);
        screen->selectChip(1, nav);  // EPUBs only
        nav.damages.clear();
    }
    const ItemRow& row(int i) { return screen->list().rows()[static_cast<size_t>(i)]; }
    Rect rowRect(int i) { return screen->list().rowRects(LibraryScreen::listArea())[static_cast<size_t>(i)]; }
    void event(SyncEvent e) {
        flow.onSyncEvent(e, nav);
        screen->onSyncEvent(e, nav);
    }
    SyncEvent download(SyncEvent::Kind k, const ItemRow& r) {
        SyncEvent e;
        e.kind = k;
        e.itemKey = r.key;
        e.attachmentKey = r.attachmentKey;
        return e;
    }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

}  // namespace

TEST_CASE("open flow: a remote book downloads with progress in its row") {
    Rig rig;
    const ItemRow r = rig.row(1);
    rig.screen->onGesture(tapAt(rig.rowRect(1).center()), rig.nav);
    REQUIRE(rig.nav.syncFake.opened.size() == 1);
    CHECK(rig.nav.syncFake.opened[0] == r.key + "/" + r.attachmentKey);
    CHECK(rig.flow.statusFor(r.attachmentKey) == "Downloading…");
    rig.nav.damages.clear();
    SyncEvent p = rig.download(SyncEvent::Kind::DownloadProgress, r);
    p.done = 42;
    p.total = 100;
    rig.event(p);
    CHECK(rig.flow.statusFor(r.attachmentKey) == "Downloading 42%");
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].waveform == Waveform::Fast);
    CHECK(rig.nav.damages[0].rect == rig.rowRect(1));
    rig.screen->onGesture(tapAt(rig.rowRect(1).center()), rig.nav);  // a second tap while downloading
    CHECK(rig.nav.syncFake.opened.size() == 1);
}

TEST_CASE("open flow: a failed download shows the error in the row; a second tap retries") {
    Rig rig;
    const ItemRow r = rig.row(0);
    rig.screen->onGesture(tapAt(rig.rowRect(0).center()), rig.nav);
    SyncEvent done = rig.download(SyncEvent::Kind::DownloadFinished, r);
    done.error = UserError{ErrorKind::NotOnServer, "This file isn't on the server yet — sync it from Zotero on your computer.",
                           "HTTP 404"};
    rig.event(done);
    CHECK(rig.flow.statusFor(r.attachmentKey) ==
          "This file isn't on the server yet — sync it from Zotero on your computer. (HTTP 404)");
    CHECK(rig.nav.pushed.empty());
    rig.screen->onGesture(tapAt(rig.rowRect(0).center()), rig.nav);
    CHECK(rig.nav.syncFake.opened.size() == 2);
}

TEST_CASE("open flow: a finished download opens the book if you are still there") {
    Rig rig;
    const ItemRow r = rig.row(2);
    rig.screen->onGesture(tapAt(rig.rowRect(2).center()), rig.nav);
    rig.lib.setFileState(r.attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    rig.event(rig.download(SyncEvent::Kind::DownloadFinished, r));
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<BookScreen*>(rig.nav.pushed[0].get()));
    CHECK(rig.flow.statusFor(r.attachmentKey).empty());
    auto last = rig.lib.lastOpened();
    REQUIRE(last);
    CHECK(last->itemKey == r.key);
    CHECK(last->openedMs == kWall);

    // Somewhere else by the time it finishes: no surprise screen change.
    Rig away;
    const ItemRow r2 = away.row(2);
    away.screen->onGesture(tapAt(away.rowRect(2).center()), away.nav);
    away.nav.visible = nullptr;
    away.event(away.download(SyncEvent::Kind::DownloadFinished, r2));
    CHECK(away.nav.pushed.empty());
}

TEST_CASE("open flow: a downloaded book opens at once and refreshes its highlights") {
    Rig rig;
    const ItemRow r = rig.row(3);
    rig.lib.setFileState(r.attachmentKey, AttachmentRecord::State::Local, "/tmp/x.epub");
    rig.screen->onShow(rig.nav);
    rig.screen->onGesture(tapAt(rig.rowRect(3).center()), rig.nav);
    CHECK(rig.nav.pushed.size() == 1);
    CHECK(rig.nav.syncFake.opened.size() == 1);
}

TEST_CASE("open flow: items without a readable file say why") {
    Rig rig;
    ItemRow none = *rig.lib.rowFor("IT000005");
    rig.flow.open(none, rig.nav);
    REQUIRE(rig.nav.toasts.size() == 1);
    CHECK(rig.nav.toasts[0] == "No EPUB or PDF for this item.");
    ItemRow linked = *rig.lib.rowFor("IT000002");
    rig.flow.open(linked, rig.nav);
    CHECK(rig.nav.toasts.back() == "This file is only on your computer.");
    CHECK(rig.nav.syncFake.opened.empty());
}

TEST_CASE("book screen: shows loading until highlights arrive") {
    Rig rig;
    ItemRow r = *rig.lib.rowFor("IT000003");
    BookScreen book(r);
    book.onShow(rig.nav);
    CHECK(book.highlights().size() == 3);  // already in the library
    ItemRow fresh = *rig.lib.rowFor("IT000018");
    BookScreen loading(fresh);
    loading.onShow(rig.nav);
    CHECK(loading.loading());
    SyncEvent e = rig.download(SyncEvent::Kind::AnnotationsReady, fresh);
    loading.onSyncEvent(e, rig.nav);
    CHECK_FALSE(loading.loading());
}

TEST_CASE("notes: newest first, grouped by item; tap opens the book") {
    Rig rig;
    NotesScreen notes;
    notes.onShow(rig.nav);
    auto lines = notes.page();
    REQUIRE(lines.size() >= 4);
    CHECK(lines[0].header);
    CHECK_FALSE(lines[1].header);
    CHECK(lines[0].itemKey == lines[1].note.itemKey);
    CHECK(lines[1].note.annotation.dateModified >= lines[2].note.annotation.dateModified);
    auto rects = notes.lineRects();
    notes.onGesture(tapAt(rects[1].center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<BookScreen*>(rig.nav.pushed[0].get()));
}

TEST_CASE("book, notes and empty notes match the reviewed goldens") {
    if (!fonts()) return;
    Rig rig;
    Canvas c(1236, 1547);
    BookScreen book(*rig.lib.rowFor("IT000003"));
    book.onShow(rig.nav);
    book.draw(c);
    test::checkGolden(c, "tests/golden/book.png");
    NotesScreen notes;
    notes.onShow(rig.nav);
    notes.draw(c);
    test::checkGolden(c, "tests/golden/notes.png");
    Db emptyDb = Db::open(":memory:");
    Library emptyLib(emptyDb);
    FakeNav emptyNav(emptyLib);
    NotesScreen none;
    none.onShow(emptyNav);
    none.draw(c);
    test::checkGolden(c, "tests/golden/notes-empty.png");
}
