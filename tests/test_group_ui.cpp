// Group libraries in the UI and the plugin API (spec: group libraries, "How it looks", "Which
// groups sync", "Plugins"): the Collections tab's group section and a group's own list, the
// Library filter's Library group, Settings → Group libraries (switches, removal only on a yes,
// Sync first, gone groups, a key without group access), the sync line, the reader in a read-only
// group and on another member's annotation, authors in note lists, and zk.library's library fields.

#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_plugins.h"
#include "fixture_library.h"
#include "plugins/host.h"
#include "screens/annotation_bar.h"
#include "screens/book_notes.h"
#include "screens/collections.h"
#include "screens/group_libraries.h"
#include "screens/library.h"
#include "screens/library_sheets.h"
#include "screens/note_popup.h"
#include "screens/notes.h"
#include "screens/reader.h"
#include "screens/settings.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <chrono>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kLab = 4242, kReaders = 777, kOld = 31;

Library::GroupMeta groupMeta(int64_t id, const std::string& name, const std::string& editing = "members") {
    Library::GroupMeta g;
    g.id = id;
    g.version = 3;
    g.name = name;
    g.libraryEditing = editing;
    g.fileEditing = editing;
    return g;
}

ItemRecord bookIn(int64_t lib, const std::string& key, const std::string& title, const std::vector<std::string>& collections = {}) {
    ItemRecord r;
    r.key = key;
    r.version = 1;
    r.itemType = "book";
    r.title = title;
    r.dateAdded = r.dateModified = "2024-05-01T00:00:00Z";
    r.json = "{}";
    r.collections = collections;
    r.lib = lib;
    return r;
}

AttachmentRecord epubOf(int64_t lib, const std::string& key, const std::string& parent) {
    AttachmentRecord a;
    a.key = key;
    a.parentKey = parent;
    a.contentType = "application/epub+zip";
    a.filename = "book.epub";
    a.linkMode = "imported_file";
    a.lib = lib;
    return a;
}

// The fixture library (My Library), and: "Lab" (editable) with a collection and two books, one
// downloaded and with another member's highlight; "Readers" (read-only) with one book; "Old"
// (gone) with one book.
void addGroups(Library& lib) {
    lib.setUser(55, "pavel");
    lib.setKeyGroupAccess(GroupAccess::All);
    lib.upsertGroup(groupMeta(kLab, "Lab"));
    lib.setGroupAccess(kLab, true, true);
    lib.upsertGroup(groupMeta(kReaders, "Readers", "admins"));
    lib.setGroupAccess(kReaders, true, true);
    lib.upsertGroup(groupMeta(kOld, "Old project"));
    lib.setGroupAccess(kOld, true, true);
    CollectionRecord c{"LABCOLL1", "Lab papers", "", 1};
    c.lib = kLab;
    lib.upsertCollection(c);
    lib.upsertItem(bookIn(kLab, "LABBOOK1", "Lab owls", {"LABCOLL1"}));
    lib.upsertItem(bookIn(kLab, "LABBOOK2", "Lab cats"));
    lib.upsertAttachment(epubOf(kLab, "LABEPUB1", "LABBOOK1"));
    lib.setFileState("LABEPUB1", AttachmentRecord::State::Local, "/nonexistent/book.epub");
    AnnotationRecord a;
    a.key = "ANNAHL01";
    a.parentKey = "LABEPUB1";
    a.type = "highlight";
    a.text = "a passage";
    a.comment = "see Kuhn";
    a.color = "#ffd400";
    a.pageLabel = "0";
    a.version = 4;
    a.lib = kLab;
    a.authorId = 99;
    a.authorName = "Anna Weber";
    a.dateModified = "2026-01-01T00:00:00Z";  // the newest: first in Notes
    lib.upsertAnnotation(a);
    lib.upsertItem(bookIn(kReaders, "READBOO1", "Read-only owls"));
    lib.upsertAttachment(epubOf(kReaders, "READEPU1", "READBOO1"));
    lib.upsertItem(bookIn(kOld, "OLDBOOK1", "Old owls"));
    lib.setGroupGone(kOld, true);
}

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    explicit Rig(bool groups = true) {
        fillFixtureLibrary(lib, 12);
        if (groups) addGroups(lib);
    }
};

bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

}  // namespace

// ---- Collections -----------------------------------------------------------------------------

TEST_CASE("group UI: with no groups the Collections tab is as before") {
    Rig rig(false);
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    CHECK(screen.groups().empty());
    CHECK_FALSE(screen.groupHeadingRect());
    CHECK(screen.groupRows().empty());
    CHECK(screen.entries().size() == 2);  // Economics, Physics
}

TEST_CASE("group UI: Collections lists the shown groups under a heading; a group opens its own list") {
    Rig rig;
    CollectionsScreen screen(nullptr);
    rig.nav.visible = &screen;
    screen.onShow(rig.nav);
    REQUIRE(screen.groups().size() == 2);  // the gone one is hidden
    CHECK(screen.groups()[0].name == "Lab");
    CHECK(screen.groups()[1].name == "Readers");
    CHECK(screen.groups()[0].items == 2);
    REQUIRE(screen.groupHeadingRect());
    CHECK(screen.groupHeadingRect()->y > screen.entryRects().back().y);  // under My Library's collections
    for (const auto& e : screen.entries()) CHECK(e.collection.lib == kMyLibrary);  // a group's aren't at the top level
    const auto rows = screen.groupRows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].second.y > screen.groupHeadingRect()->y);

    screen.onGesture(tapAt(rows[0].second.center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* list = dynamic_cast<CollectionScreen*>(rig.nav.pushed[0].get());
    REQUIRE(list);
    CHECK(list->group() == kLab);
    list->onShow(rig.nav);
    REQUIRE(list->page().size() == 3);  // its collection, then all its items
    CHECK(list->page()[0].isCollection);
    CHECK(list->page()[0].collection.collection.name == "Lab papers");
    CHECK(list->page()[1].item.title == "Lab cats");
    CHECK(list->page()[2].item.title == "Lab owls");

    // Keep downloaded, from the long press, works for a group's collection.
    list->onGesture(Gesture{GestureType::LongPress, list->lineRect(0).center(), list->lineRect(0).center()}, rig.nav);
    REQUIRE(list->keepSheetLabel());
    CHECK(*list->keepSheetLabel() == "Keep downloaded");
    list->onGesture(tapAt(CollectionScreen::keepButtonRect().center()), rig.nav);
    CHECK(has(rig.lib.keptCollections(), "LABCOLL1"));
    CHECK(rig.nav.syncFake.keptRuns == 1);
}

TEST_CASE("group UI: a switched-off group leaves the Collections tab") {
    Rig rig;
    rig.lib.setLibraryEnabled(kReaders, false);
    CollectionsScreen screen(nullptr);
    screen.onShow(rig.nav);
    REQUIRE(screen.groups().size() == 1);
    CHECK(screen.groups()[0].id == kLab);
    rig.lib.setLibraryEnabled(kLab, false);
    screen.onShow(rig.nav);
    CHECK(screen.groups().empty());
    CHECK_FALSE(screen.groupHeadingRect());
}

// ---- Library filter --------------------------------------------------------------------------

TEST_CASE("group UI: the Filter sheet has a Library group only when there are groups") {
    Rig plain(false);
    FilterSheet none(LibraryView{}, plain.lib.libraries(false));
    CHECK_FALSE(none.hasLibraries());
    CHECK(none.libraryRects({0, 140, 1236, 1277}).empty());

    Rig rig;
    const Rect area = LibraryScreen::sheetArea();
    FilterSheet sheet(LibraryView{}, rig.lib.libraries(false));
    REQUIRE(sheet.hasLibraries());
    CHECK(sheet.libraryLabels() == std::vector<std::string>{"All", "My Library", "Lab", "Readers"});
    CHECK(sheet.sheetRect(area).h > none.sheetRect(area).h);
    const auto rects = sheet.libraryRects(area);
    REQUIRE(rects.size() == 4);
    CHECK(sheet.onGesture(tapAt(rects[2].center()), area) == FilterSheet::Action::Changed);
    CHECK(sheet.draft().lib == kLab);
    CHECK(sheet.draft().filtered());
    CHECK(sheet.draft().cleared().lib == kAllLibraries);
    // Type and the rest still answer below it.
    CHECK(sheet.onGesture(tapAt(sheet.typeRects(area)[2].center()), area) == FilterSheet::Action::Changed);
    CHECK(sheet.draft().type == TypeFilter::Pdf);
}

TEST_CASE("group UI: the Library filter names the library, is remembered, and falls back when the group goes") {
    Rig rig;
    LibraryScreen screen(nullptr);
    rig.nav.visible = &screen;
    screen.onShow(rig.nav);
    LibraryView v = screen.view();
    v.type = TypeFilter::Any;
    v.lib = kLab;
    screen.setView(v, rig.nav);
    CHECK(screen.list().pager().total() == 2);
    CHECK(screen.filterLine() == "Lab · All types · 2 items");
    CHECK(loadLibraryView(rig.lib).lib == kLab);  // remembered on this Kindle

    LibraryScreen again(nullptr);
    again.onShow(rig.nav);
    CHECK(again.view().lib == kLab);
    CHECK(again.list().pager().total() == 2);

    rig.lib.setLibraryEnabled(kLab, false);  // switched off in Settings meanwhile
    again.onShow(rig.nav);
    CHECK(again.view().lib == kAllLibraries);
    CHECK(loadLibraryView(rig.lib).lib == kAllLibraries);
    CHECK(again.filterLine().find("Lab") == std::string::npos);
}

TEST_CASE("group UI: a view saved before groups loads as All") {
    Rig rig;
    rig.lib.setMetaValue("libraryView", R"({"downloaded":false,"type":"any","read":"any","tag":"","sort":"title"})");
    CHECK(loadLibraryView(rig.lib).lib == kAllLibraries);
    LibraryView v;
    v.lib = kMyLibrary;
    saveLibraryView(rig.lib, v);
    CHECK(loadLibraryView(rig.lib).lib == kMyLibrary);
    CHECK(filterLine(v, 3, "My Library") == "My Library · 3 items");
}

// ---- Sync line -------------------------------------------------------------------------------

TEST_CASE("group UI: the sync line names the group being synced") {
    SyncStatus st;
    SyncEvent start;
    start.kind = SyncEvent::Kind::SyncStarted;
    st.onEvent(start, 0);
    SyncEvent p;
    p.kind = SyncEvent::Kind::SyncProgress;
    p.progress.done = 1200;
    p.progress.total = 6560;
    st.onEvent(p, 0);
    CHECK(st.line(0) == "Syncing… 1,200 of 6,560");
    p.progress.lib = kLab;
    p.progress.library = "Lab";
    st.onEvent(p, 0);
    CHECK(st.line(0) == "Syncing Lab… 1,200 of 6,560");
    p.progress.total = 0;
    st.onEvent(p, 0);
    CHECK(st.line(0) == "Syncing Lab…");
}

// ---- Settings --------------------------------------------------------------------------------

TEST_CASE("group UI: Settings has a Group libraries row only with groups or a key that can't read them") {
    Config c;
    c.userId = 1;
    c.webdavUrl = "https://dav.example.org/zotero/";
    {
        Rig rig(false);
        SettingsScreen s(c, [](Nav&) {});
        s.onShow(rig.nav);
        CHECK_FALSE(s.groupsRow());
        CHECK(s.accountLine() == "User ID 1");
        rig.lib.setKeyGroupAccess(GroupAccess::None);
        s.onShow(rig.nav);
        CHECK(s.groupsRow());
        CHECK(s.groupsLine() == "Your API key can't read group libraries");
    }
    Rig rig;
    SettingsScreen s(c, [](Nav&) {});
    s.onShow(rig.nav);
    REQUIRE(s.groupsRow());
    CHECK(s.accountLine() == "User ID 1 · WebDAV: dav.example.org");
    CHECK(s.groupsLine() == "2 on · 1 no longer available");
    s.onGesture(tapAt(s.groupsRect().center()), rig.nav);
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<GroupLibrariesScreen*>(rig.nav.pushed[0].get()));
}

TEST_CASE("group UI: Group libraries lists every group with its state") {
    Rig rig;
    GroupLibrariesScreen s;
    s.onShow(rig.nav);
    REQUIRE(s.rows().size() == 3);  // by name: Lab, Old project, Readers
    CHECK(s.note(s.rows()[0]) == "2 items");
    CHECK(s.note(s.rows()[1]) == "No longer available · still on this Kindle");
    CHECK(s.note(s.rows()[2]) == "1 item · read-only");
    CHECK_FALSE(s.toggleRect(0).empty());
    CHECK(s.toggleRect(1).empty());  // gone: no switch
    CHECK(s.removeRect(0).empty());
    CHECK_FALSE(s.removeRect(1).empty());  // gone: Remove from Kindle
    CHECK_FALSE(s.accessLineRect());
    rig.lib.setKeyGroupAccess(GroupAccess::None);
    s.onShow(rig.nav);
    REQUIRE(s.accessLineRect());
    CHECK(s.rowRect(0).y > s.accessLineRect()->y);
}

TEST_CASE("group UI: turning a group off hides it at once; its copy goes only on a yes, off the UI thread") {
    Rig rig;
    GroupLibrariesScreen s;
    rig.nav.visible = &s;
    s.onShow(rig.nav);
    s.onGesture(tapAt(s.toggleRect(0).center()), rig.nav);  // Lab off
    CHECK_FALSE(rig.lib.libraryInfo(kLab)->enabled);
    CHECK(rig.lib.syncedGroups() == std::vector<int64_t>{kReaders});
    REQUIRE(s.sheet());
    CHECK(s.sheet()->kind == GroupSheet::Kind::Remove);
    CHECK(s.sheet()->title() == "Remove Lab from this Kindle?");
    CHECK(s.sheet()->body().find("1 book from it is downloaded on this Kindle") != std::string::npos);
    CHECK(s.sheet()->body().find("nothing changes in Zotero") != std::string::npos);
    CHECK(GroupSheet::label(s.sheet()->primary()) == "Remove from Kindle");
    CHECK(GroupSheet::label(s.sheet()->secondary()) == "Keep");
    // Keep: nothing removed; the row offers it later.
    s.onGesture(tapAt(s.sheetButtonRect(1).center()), rig.nav);
    CHECK_FALSE(s.sheet());
    CHECK(rig.nav.syncFake.removedLibraries.empty());
    CHECK(s.note(s.rows()[0]) == "Off · still on this Kindle");
    REQUIRE_FALSE(s.removeRect(0).empty());
    // Remove from Kindle: asked again, then the worker does it.
    s.onGesture(tapAt(s.removeRect(0).center()), rig.nav);
    REQUIRE(s.sheet());
    s.onGesture(tapAt(s.sheetButtonRect(0).center()), rig.nav);
    REQUIRE(rig.nav.syncFake.removedLibraries.size() == 1);
    CHECK(rig.nav.syncFake.removedLibraries[0] == std::pair<int64_t, bool>{kLab, false});
    CHECK(s.note(s.rows()[0]) == "Removing from this Kindle…");
    CHECK(rig.lib.libraryInfo(kLab)->items == 2);  // not by the UI thread
    // Turning it on again syncs it.
    rig.lib.removeLibraryData(kLab);  // (what the worker does)
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    s.onSyncEvent(done, rig.nav);
    CHECK(s.note(s.rows()[0]) == "Off");
    s.onGesture(tapAt(s.toggleRect(0).center()), rig.nav);
    CHECK(rig.lib.libraryInfo(kLab)->enabled);
    CHECK(rig.nav.syncFake.syncs == 1);
    CHECK_FALSE(s.sheet());
}

TEST_CASE("group UI: a group with unsent changes asks first and offers Sync first") {
    Rig rig;
    AnnotationRecord mine;
    mine.parentKey = "LABEPUB1";
    mine.type = "highlight";
    mine.text = "mine";
    mine.color = "#ffd400";
    rig.lib.createAnnotation(mine);
    GroupLibrariesScreen s;
    rig.nav.visible = &s;
    s.onShow(rig.nav);
    s.onGesture(tapAt(s.toggleRect(0).center()), rig.nav);
    CHECK(rig.lib.libraryInfo(kLab)->enabled);  // not yet
    REQUIRE(s.sheet());
    CHECK(s.sheet()->kind == GroupSheet::Kind::TurnOff);
    CHECK(s.sheet()->body().find("1 change made on this Kindle hasn't been sent to Zotero yet") != std::string::npos);
    CHECK(GroupSheet::label(s.sheet()->primary()) == "Sync first");
    s.onGesture(tapAt(s.sheetButtonRect(0).center()), rig.nav);  // Sync first
    CHECK(rig.nav.syncFake.syncs == 1);
    CHECK(s.note(s.rows()[0]) == "Syncing first…");
    // The sync sent it: off now, and the copy is asked about.
    for (auto& a : rig.lib.dirtyAnnotations(kLab)) {
        a.dirty = false;
        a.version = 9;
        rig.lib.upsertAnnotation(a);
    }
    REQUIRE(rig.lib.libraryRemovalCheck(kLab).unsent == 0);
    SyncEvent done;
    done.kind = SyncEvent::Kind::SyncFinished;
    s.onSyncEvent(done, rig.nav);
    CHECK_FALSE(rig.lib.libraryInfo(kLab)->enabled);
    REQUIRE(s.sheet());
    CHECK(s.sheet()->kind == GroupSheet::Kind::Remove);
    // A tap outside keeps it.
    s.onGesture(tapAt({600, 200}), rig.nav);
    CHECK_FALSE(s.sheet());
    CHECK(rig.nav.syncFake.removedLibraries.empty());
}

TEST_CASE("group UI: turning off anyway keeps unsent changes; a gone group's removal says they'd be lost") {
    Rig rig;
    rig.lib.markRead("OLDBOOK1", true);  // a change in the gone group, never to be sent
    rig.lib.markRead("LABBOOK2", true);
    GroupLibrariesScreen s;
    s.onShow(rig.nav);
    s.onGesture(tapAt(s.toggleRect(0).center()), rig.nav);
    REQUIRE(s.sheet());
    s.onGesture(tapAt(s.sheetButtonRect(1).center()), rig.nav);  // Turn off
    CHECK_FALSE(rig.lib.libraryInfo(kLab)->enabled);
    CHECK_FALSE(s.sheet());
    CHECK(rig.nav.syncFake.removedLibraries.empty());

    s.onGesture(tapAt(s.removeRect(1).center()), rig.nav);  // Old project
    REQUIRE(s.sheet());
    const GroupSheet& sh = *s.sheet();
    CHECK(sh.body().find("It's no longer available to you.") == 0);
    CHECK(sh.body().find("can't be sent any more") != std::string::npos);
    CHECK(GroupSheet::label(sh.primary()) == "Keep");
    CHECK(GroupSheet::label(sh.secondary()) == "Remove anyway");
    s.onGesture(tapAt(s.sheetButtonRect(1).center()), rig.nav);
    REQUIRE(rig.nav.syncFake.removedLibraries.size() == 1);
    CHECK(rig.nav.syncFake.removedLibraries[0] == std::pair<int64_t, bool>{kOld, true});
}

TEST_CASE("group UI: the summary line counts groups by state") {
    std::vector<LibraryInfo> libs(4);
    libs[1].id = 1;
    libs[2].id = 2;
    libs[2].enabled = false;
    libs[3].id = 3;
    libs[3].gone = true;
    CHECK(groupLibrariesSummary(libs, GroupAccess::All) == "1 on · 1 off · 1 no longer available");
    CHECK(groupLibrariesSummary({libs[0]}, GroupAccess::Unknown) == "None yet: groups you're in appear after a sync");
}

// ---- Authors and read-only annotations -------------------------------------------------------

TEST_CASE("group UI: another member's annotation names them and can't be changed") {
    Rig rig;
    auto anna = *rig.lib.annotation("ANNAHL01");
    CHECK(annotationAuthor(rig.lib, anna) == "Anna Weber");
    CHECK(annotationReadOnly(rig.lib, anna) == "by Anna Weber");
    CHECK(noteRowQuietLine(anna, "Anna Weber") == "by Anna Weber");
    anna.tags = {"method"};
    CHECK(noteRowQuietLine(anna, "Anna Weber") == "by Anna Weber · #method");
    auto mine = anna;
    mine.authorId = 55;  // the user's own, made in Zotero
    CHECK(annotationAuthor(rig.lib, mine) == "");
    CHECK(annotationReadOnly(rig.lib, mine) == "");
    auto ro = anna;
    ro.lib = kReaders;
    ro.authorId = 55;
    CHECK(annotationReadOnly(rig.lib, ro) == kReadOnlyGroup);
    auto home = *rig.lib.annotationsFor("EP000000").begin();
    CHECK(annotationAuthor(rig.lib, home) == "");

    // The editor shows only "by Anna Weber"; taps on it change nothing, Comment and Delete don't open.
    AnnotationEditor e(anna, {100, 600, 600, 50}, {}, annotationReadOnly(rig.lib, anna));
    CHECK(e.readOnly());
    CHECK(e.bar().mode() == AnnotationBar::Mode::View);
    CHECK(e.bar().label() == "by Anna Weber");
    CHECK_FALSE(e.bar().hasColours());
    CHECK(e.onTap(e.bar().rect().center(), rig.nav) == AnnotationEditor::Result::Open);
    e.editComment(rig.nav);
    CHECK_FALSE(e.commenting());
    e.confirmDelete(rig.nav);
    CHECK_FALSE(e.confirming());
    CHECK_FALSE(rig.lib.annotation("ANNAHL01")->dirty);
    CHECK(e.onTap({5, 5}, rig.nav) == AnnotationEditor::Result::Closed);

    // A sticky note of theirs: Close only.
    auto note = anna;
    note.type = "note";
    NotePopup popup(note, {300, 300, 60, 60}, ReaderScreen::pageArea(), {"x"}, "by Anna Weber");
    CHECK(popup.readOnly());
    CHECK(popup.onGesture(tapAt(popup.buttonRect(0).center()), rig.nav) == NotePopup::Action::None);
    CHECK(popup.onGesture(tapAt(popup.buttonRect(2).center()), rig.nav) == NotePopup::Action::Close);
}

TEST_CASE("group UI: the Notes tab and a book's notes name another member under the comment") {
    Rig rig;
    rig.lib.setAllAnnotationsOffline(true);  // the Notes tab lists them all
    NotesScreen notes;
    notes.onShow(rig.nav);
    bool found = false;
    for (const auto& l : notes.page())
        if (!l.header && l.note.annotation.key == "ANNAHL01") {
            found = true;
            CHECK(l.author == "Anna Weber");
        } else if (!l.header) {
            CHECK(l.author.empty());
        }
    CHECK(found);
    // Book notes: a long press opens the read-only bar.
    BookNotesScreen book(*rig.lib.rowFor("LABBOOK1"), nullptr);
    book.onShow(rig.nav);
    REQUIRE(book.shown().size() == 1);
    const Rect row = book.rowRects()[0];
    book.onGesture(Gesture{GestureType::LongPress, row.center(), row.center()}, rig.nav);
    REQUIRE(book.annotationEditor());
    CHECK(book.annotationEditor()->readOnly());
}

// ---- The reader ------------------------------------------------------------------------------

namespace {

struct ReaderRig {
    Rig rig;
    std::unique_ptr<ReaderScreen> reader;
    explicit ReaderRig(const std::string& itemKey, DocType type = DocType::Epub) {
        addFontDir("assets/fonts-device");
        reader = std::make_unique<ReaderScreen>(*rig.lib.rowFor(itemKey), "book.epub", type, [] {
            return std::make_unique<RenderWorker>([](DocType) { return std::unique_ptr<DocumentEngine>(std::make_unique<FakeEngine>()); });
        }, 6);
        rig.nav.visible = reader.get();
        reader->onShow(rig.nav);
        REQUIRE(waitFor([&] { return reader->ready(); }));
    }
    bool waitFor(const std::function<bool()>& done) {
        for (int i = 0; i < 400; ++i) {
            reader->onTick(rig.nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool select(Point p) {
        reader->onGesture(Gesture{GestureType::LongPress, p, p}, rig.nav);
        reader->onGesture(Gesture{GestureType::LongPressUp, p, p}, rig.nav);
        return waitFor([&] { return reader->annotationBar() != nullptr; });
    }
};

}  // namespace

TEST_CASE("group UI: in a read-only group the selection offers Search only, and the first try says why") {
    ReaderRig r("READBOO1");
    CHECK(r.reader->readOnlyLibrary());
    REQUIRE(r.select({150, 500}));
    CHECK(r.reader->annotationBar()->mode() == AnnotationBar::Mode::Text);
    CHECK(r.rig.nav.toasts == std::vector<std::string>{"This group library is read-only."});
    r.reader->onGesture(tapAt({1100, 1400}), r.rig.nav);  // closes it
    REQUIRE(r.select({250, 500}));
    CHECK(r.rig.nav.toasts.size() == 1);  // once
    // Mark an area and Add note say so every time, and don't arm.
    r.reader->onGesture(tapAt({1100, 1400}), r.rig.nav);
    r.reader->markArea(r.rig.nav);
    CHECK_FALSE(r.reader->areaArmed());
    r.reader->addNote(r.rig.nav);
    CHECK_FALSE(r.reader->noteArmed());
    CHECK(r.rig.nav.toasts.size() == 1);  // EPUB: neither exists there
}

TEST_CASE("group UI: a read-only group's PDF takes no area, note or ink") {
    ReaderRig r("READBOO1", DocType::Pdf);
    r.reader->markArea(r.rig.nav);
    CHECK_FALSE(r.reader->areaArmed());
    CHECK(r.rig.nav.toasts.size() == 1);
    r.reader->addNote(r.rig.nav);
    CHECK_FALSE(r.reader->noteArmed());
    CHECK(r.rig.nav.toasts.size() == 2);
    CHECK_FALSE(static_cast<Screen*>(r.reader.get())->wantsSystemInk());
    PenEvent down;
    down.type = PenEvent::Type::Down;
    down.pos = {600, 900};
    CHECK_FALSE(r.reader->onPen(down, r.rig.nav));  // a finger, not ink
    PenEvent up = down;
    up.type = PenEvent::Type::Up;
    r.reader->onPen(up, r.rig.nav);
    CHECK(r.reader->pendingInkStrokes() == 0);
    CHECK(r.rig.lib.annotationsFor("READEPU1").empty());
}

TEST_CASE("group UI: in an editable group the reader highlights; another member's highlight opens as \"by\"") {
    ReaderRig r("LABBOOK1");
    CHECK_FALSE(r.reader->readOnlyLibrary());
    REQUIRE(r.waitFor([&] { return !r.reader->highlights().empty(); }));
    r.reader->onGesture(tapAt({400, 225}), r.rig.nav);  // Anna's highlight
    REQUIRE(r.reader->annotationEditor());
    CHECK(r.reader->annotationEditor()->readOnly());
    CHECK(r.reader->annotationEditor()->bar().label() == "by Anna Weber");
    r.reader->onGesture(tapAt(r.reader->annotationEditor()->bar().rect().center()), r.rig.nav);
    CHECK_FALSE(r.rig.lib.annotation("ANNAHL01")->dirty);
    r.reader->onGesture(tapAt({1100, 1400}), r.rig.nav);
    CHECK_FALSE(r.reader->annotationEditor());
    REQUIRE(r.select({150, 500}));
    CHECK(r.reader->annotationBar()->mode() == AnnotationBar::Mode::Create);
    CHECK(r.rig.nav.toasts.empty());
}

// ---- Plugins ---------------------------------------------------------------------------------

TEST_CASE("group UI: a plugin change to a switched-off or gone group says so") {
    Rig rig;
    rig.lib.setLibraryEnabled(kLab, false);
    PluginChange ch;
    ch.pluginId = "p";
    ch.kind = PluginChange::Kind::AddTag;
    ch.itemKey = "LABBOOK1";
    ch.value = "x";
    std::string why;
    CHECK(rig.lib.queuePluginChange(ch, &why).empty());
    CHECK(why.find("\"Lab\" is switched off on this Kindle") != std::string::npos);
    rig.lib.setLibraryEnabled(kLab, true);
    rig.lib.setGroupGone(kLab, true);
    why.clear();
    CHECK(rig.lib.queuePluginChange(ch, &why).empty());
    CHECK(why.find("\"Lab\" is no longer available") != std::string::npos);
    ch.itemKey = "READBOO1";
    why.clear();
    CHECK(rig.lib.queuePluginChange(ch, &why).empty());
    CHECK(why.find("\"Readers\" is read-only") != std::string::npos);
}
