// 0.7 part A: tags on annotations. The annotation bar's tag row (the four tags put on annotations
// most recently, then + Tag), the same row on an area's bar and a sticky note's pop-up, the tag
// picker with "Add “xyz”", tags on the Notes and book notes rows, and their Tag filter. Tag edits
// go to Zotero through the usual annotation push and conflict path.

#include <doctest/doctest.h>

#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fake_zotero.h"
#include "fixture_library.h"
#include "golden.h"
#include "net/write_guard.h"
#include "screens/annotation_bar.h"
#include "screens/book_notes.h"
#include "screens/note_popup.h"
#include "screens/notes.h"
#include "screens/read_tag.h"
#include "screens/reader.h"
#include "screens/search.h"
#include "store/db.h"
#include "sync/sync_engine.h"
#include "ui/metrics.h"
#include "ui/paint.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "zotero/api.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace zk;
using namespace zk::test;
using json = nlohmann::json;

namespace {

constexpr Point kOnBox{400, 225};  // FakeEngine draws a page's annotations at {100, 200, 600, 50}

// The reader on a fixture book with one highlight from Zotero on its first page.
struct ReaderRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;

    explicit ReaderRig(DocType type = DocType::Epub) {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        for (auto& a : lib.annotationsFor("EP000003")) lib.removeObject("annotation", a.key);
        AnnotationRecord a;
        a.key = "TAGME001";
        a.parentKey = "EP000003";
        a.type = "highlight";
        a.text = "a passage";
        a.color = "#ffd400";
        a.pageLabel = "0";
        a.version = 5;
        lib.upsertAnnotation(a);
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), type == DocType::Pdf ? "book.pdf" : "book.epub", type, [] {
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
    void select(Point p) {
        reader->onGesture(Gesture{GestureType::LongPress, p, p}, nav);
        reader->onGesture(Gesture{GestureType::LongPressUp, p, p}, nav);
        REQUIRE(waitFor([&] { return reader->annotationBar() != nullptr; }));
    }
    void annotationsReady() {
        SyncEvent ready;
        ready.kind = SyncEvent::Kind::AnnotationsReady;
        ready.attachmentKey = "EP000003";
        reader->onSyncEvent(ready, nav);
    }
    std::vector<std::string> tags(const std::string& key) { return lib.annotation(key)->tags; }
    // The picker + Tag pushed (the last screen pushed), shown.
    TagPickerScreen* picker() {
        REQUIRE_FALSE(nav.pushed.empty());
        auto* p = dynamic_cast<TagPickerScreen*>(nav.pushed.back().get());
        REQUIRE(p);
        p->onShow(nav);
        return p;
    }
    void remember(std::initializer_list<const char*> oldestFirst) {
        for (const char* t : oldestFirst) lib.rememberAnnotationTag(t);
    }
};

void type(Screen& s, FakeNav& nav, const std::string& text) {
    for (char c : text) s.onKey({KeyEvent::Code::Text, std::string(1, c)}, nav);
}

// The sync side: a library synced with a fake Zotero, as test_annotation_sync's.
struct SyncRig {
    FakeZotero server;
    Db db{Db::open(":memory:")};
    Library lib{db};
    WriteGuard guard{server, [this](const std::string& k) { return lib.annotation(k).has_value(); },
                     [this](const std::string& k) { return lib.attachment(k).has_value(); }};
    ZoteroApi api{guard, 1, "k3y"};
    SyncEngine engine{api, lib};
    FakeNav nav{lib};

    SyncRig() {
        api.setSleeper([](int) {});
        lib.setAllAnnotationsOffline(true);
        server.put(FakeZotero::item("BOOK0001", "A book"));
        server.put(FakeZotero::attachment("EPUB0001", "BOOK0001", "md5-one"));
        server.put(FakeZotero::annotation("ANNO0001", "EPUB0001", "desktop text"));
        REQUIRE_FALSE(pull().error);
    }
    SyncReport pull() { return engine.pull([](const SyncProgress&) {}); }
    std::vector<std::string> serverTags(const std::string& key) {
        std::vector<std::string> out;
        for (const auto& t : server.items.at(key)["data"].value("tags", json::array())) out.push_back(t.value("tag", ""));
        return out;
    }
};

bool inside(Rect r, Rect in) { return in.intersected(r) == r; }

}  // namespace

// ---- the chips -----------------------------------------------------------------------------------

TEST_CASE("annotation tags: a chip toggles its tag on the annotation, and the change is queued for Zotero") {
    ReaderRig rig;
    rig.remember({"later", "to-cite", "method"});
    rig.tap(kOnBox);
    AnnotationEditor* editor = rig.reader->annotationEditor();
    REQUIRE(editor);
    const TagRow& row = editor->bar().tags();
    REQUIRE(row.chips() == std::vector<std::string>{"method", "to-cite", "later"});
    CHECK_FALSE(row.isOn("method"));
    const int pushes = rig.nav.syncFake.pushes;
    rig.nav.damages.clear();
    rig.tap(row.chipRect(0).center());
    REQUIRE(rig.reader->annotationEditor());  // the bar stays open
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"method"});
    CHECK(rig.lib.annotation("TAGME001")->dirty);
    CHECK(rig.lib.annotation("TAGME001")->version == 5);  // an edit of Zotero's version 5
    CHECK(rig.reader->annotationEditor()->bar().tags().isOn("method"));  // the chip is filled
    CHECK(rig.nav.syncFake.pushes > pushes);
    bool repainted = false;
    for (const auto& d : rig.nav.damages) repainted |= d.waveform == Waveform::Ui && inside(row.chipRect(0), d.rect);
    CHECK(repainted);
    // The chips keep their places (the tag is now the most recent, but the row doesn't move under the finger).
    CHECK(rig.reader->annotationEditor()->bar().tags().chips() == std::vector<std::string>{"method", "to-cite", "later"});
    rig.tap(rig.reader->annotationEditor()->bar().tags().chipRect(1).center());
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"method", "to-cite"});
    rig.tap(rig.reader->annotationEditor()->bar().tags().chipRect(0).center());  // off again
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"to-cite"});
    CHECK_FALSE(rig.reader->annotationEditor()->bar().tags().isOn("method"));
    // A plugin hears of each change as a highlight change.
    CHECK(rig.nav.pluginSink.named("highlightChanged").size() == 3);
}

TEST_CASE("annotation tags: a chip's change is sent as the annotation's tags in Zotero's JSON, and comes back") {
    SyncRig rig;
    rig.lib.rememberAnnotationTag("method");
    AnnotationEditor editor(*rig.lib.annotation("ANNO0001"), {100, 600, 600, 50}, recentChips(rig.lib));
    rig.nav.visible = nullptr;
    editor.onTap(editor.bar().tags().chipRect(0).center(), rig.nav);
    CHECK(editor.takeChanged());  // the host pushes soon
    REQUIRE(rig.lib.dirtyAnnotations().size() == 1);
    // What goes up: Zotero's tags array.
    const json up = annotationJson(*rig.lib.annotation("ANNO0001"));
    CHECK(up["tags"] == json::parse(R"([{"tag":"method"}])"));
    rig.server.sent.clear();
    REQUIRE_FALSE(rig.engine.push().error);
    const json body = json::parse(rig.server.sent.back().body);
    CHECK(body[0]["key"] == "ANNO0001");
    CHECK(body[0]["tags"] == json::parse(R"([{"tag":"method"}])"));
    CHECK(rig.serverTags("ANNO0001") == std::vector<std::string>{"method"});
    CHECK_FALSE(rig.lib.annotation("ANNO0001")->dirty);
    // Zotero's copy comes back with the tag (a later desktop edit keeps it).
    json data = rig.server.items.at("ANNO0001")["data"];
    data["tags"].push_back({{"tag", "desktop"}});
    rig.server.put(data);
    REQUIRE_FALSE(rig.pull().error);
    CHECK(rig.lib.annotation("ANNO0001")->tags == std::vector<std::string>{"method", "desktop"});
}

TEST_CASE("annotation tags: tags changed here and in Zotero at once go through the conflict path") {
    SyncRig rig;
    rig.lib.rememberAnnotationTag("kindle-tag");
    AnnotationEditor editor(*rig.lib.annotation("ANNO0001"), {100, 600, 600, 50}, recentChips(rig.lib));
    editor.onTap(editor.bar().tags().chipRect(0).center(), rig.nav);
    json data = rig.server.items.at("ANNO0001")["data"];  // the desktop tags it meanwhile
    data["tags"] = json::parse(R"([{"tag":"desktop-tag"}])");
    rig.server.put(data);
    const SyncReport rep = rig.engine.push();  // 412 -> pull -> the Kindle edit becomes a conflict
    CHECK_FALSE(rep.error);
    CHECK(rig.serverTags("ANNO0001") == std::vector<std::string>{"desktop-tag"});
    CHECK(rig.lib.annotation("ANNO0001")->tags == std::vector<std::string>{"desktop-tag"});
    auto conflicts = rig.lib.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].conflictOf == "ANNO0001");
    CHECK(conflicts[0].tags == std::vector<std::string>{"kindle-tag"});
    // Re-apply puts the Kindle's tags back on it, sent as an edit of Zotero's new version.
    CHECK(rig.lib.reapplyConflict(conflicts[0].key) == "ANNO0001");
    REQUIRE_FALSE(rig.engine.push().error);
    CHECK(rig.serverTags("ANNO0001") == std::vector<std::string>{"kindle-tag"});
}

TEST_CASE("annotation tags: recent tags fill the chips, most recent first, and are kept on the Kindle") {
    Db db{Db::open(":memory:")};
    {
        Library lib(db);
        CHECK(lib.recentAnnotationTags().empty());
        CHECK(recentChips(lib).empty());
        for (const char* t : {"a", "b", "c", "d", "e"}) lib.rememberAnnotationTag(t);
        lib.rememberAnnotationTag("b");  // again: to the front, once
        CHECK(lib.recentAnnotationTags() == std::vector<std::string>{"b", "e", "d", "c", "a"});
        CHECK(recentChips(lib) == std::vector<std::string>{"b", "e", "d", "c"});
        for (int i = 0; i < 20; ++i) lib.rememberAnnotationTag("t" + std::to_string(i));
        CHECK(lib.recentAnnotationTags().size() == Library::kRecentAnnotationTags);
        CHECK(lib.recentAnnotationTags().front() == "t19");
    }
    Library again(db);  // the app started again
    CHECK(recentChips(again) == std::vector<std::string>{"t19", "t18", "t17", "t16"});
}

TEST_CASE("annotation tags: on a new selection a chip marks the tag; Highlight makes it with the tag, now the most recent") {
    ReaderRig rig;
    rig.remember({"later", "method"});
    rig.select({130, 510});
    AnnotationBar* bar = rig.reader->annotationBar();
    REQUIRE(bar->tags().chips() == std::vector<std::string>{"method", "later"});
    rig.tap(bar->tags().chipRect(1).center());  // later
    REQUIRE(rig.reader->annotationBar());
    CHECK(rig.reader->annotationBar()->tags().isOn("later"));
    CHECK(rig.lib.annotationsFor("EP000003").size() == 1);  // nothing made yet
    rig.tap(rig.reader->annotationBar()->buttonRect(0).center());  // Highlight
    std::optional<AnnotationRecord> made;
    for (auto& a : rig.lib.annotationsFor("EP000003"))
        if (a.createdHere) made = a;
    REQUIRE(made);
    CHECK(made->tags == std::vector<std::string>{"later"});
    CHECK(annotationJson(*made)["tags"] == json::parse(R"([{"tag":"later"}])"));
    CHECK(rig.lib.recentAnnotationTags() == std::vector<std::string>{"later", "method"});
    // The next selection starts with nothing marked.
    rig.select({130, 630});
    CHECK(rig.reader->annotationBar()->tags().on().empty());
    CHECK(rig.reader->annotationBar()->tags().chips() == std::vector<std::string>{"later", "method"});
}

TEST_CASE("annotation tags: the tags marked stay for a Note made from the bar") {
    ReaderRig rig;
    rig.remember({"question"});
    rig.select({130, 510});
    rig.tap(rig.reader->annotationBar()->tags().chipRect(0).center());
    rig.tap(rig.reader->annotationBar()->buttonRect(2).center());  // Note
    REQUIRE(rig.reader->noteOpen());
    type(*rig.reader, rig.nav, "why");
    rig.reader->onKey({KeyEvent::Code::Return, ""}, rig.nav);
    std::optional<AnnotationRecord> made;
    for (auto& a : rig.lib.annotationsFor("EP000003"))
        if (a.createdHere) made = a;
    REQUIRE(made);
    CHECK(made->comment == "why");
    CHECK(made->tags == std::vector<std::string>{"question"});
}

// ---- + Tag ---------------------------------------------------------------------------------------

TEST_CASE("annotation tags: + Tag opens the picker: the library's tags, recent first; searching and picking one adds it") {
    ReaderRig rig;
    rig.remember({"zeta-recent"});
    AnnotationRecord other = *rig.lib.annotation("TAGME001");
    other.key = "OTHER001";
    other.tags = {"method", "method-two"};
    rig.lib.upsertAnnotation(other);
    rig.tap(kOnBox);
    REQUIRE(rig.reader->annotationEditor());
    rig.tap(rig.reader->annotationEditor()->bar().tags().addRect().center());
    TagPickerScreen* p = rig.picker();
    CHECK(p->isReading());  // turns with the reader
    CHECK(rig.nav.keyboardFake.shown);
    const auto& all = p->suggestions();
    REQUIRE(all.size() >= 4);
    CHECK(all[0].first == "zeta-recent");  // recent first
    CHECK(all[1].first == "method");       // then those on annotations
    bool itemTag = false;                   // and the library's item tags
    for (const auto& s : all) itemTag |= s.first == "economics";
    CHECK(itemTag);
    type(*p, rig.nav, "meth");
    REQUIRE(p->suggestions().size() == 2);
    CHECK(p->newTag() == "meth");  // not a tag yet: offered as new
    CHECK_FALSE(p->addRect().empty());
    type(*p, rig.nav, "od");
    CHECK(p->newTag().empty());  // "method" exists: no Add row
    CHECK(p->addRect().empty());
    p->onGesture(tapAt(p->suggestionRects()[0].center()), rig.nav);
    CHECK(rig.nav.pops == 1);
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"method"});
    CHECK(rig.lib.recentAnnotationTags().front() == "method");
    // Back in the reader the bar shows it filled, leading the chips.
    rig.reader->onShow(rig.nav);
    REQUIRE(rig.reader->annotationEditor());
    CHECK(rig.reader->annotationEditor()->bar().tags().chips().front() == "method");
    CHECK(rig.reader->annotationEditor()->bar().tags().isOn("method"));
}

TEST_CASE("annotation tags: + Tag adds a new tag: Add “xyz”, or Return") {
    ReaderRig rig;
    rig.tap(kOnBox);
    rig.tap(rig.reader->annotationEditor()->bar().tags().addRect().center());  // no recent tags: + Tag alone
    TagPickerScreen* p = rig.picker();
    type(*p, rig.nav, "  to cite ");
    CHECK(p->newTag() == "to cite");
    REQUIRE_FALSE(p->addRect().empty());
    CHECK(p->addRect().y < (p->suggestionRects().empty() ? 1 << 20 : p->suggestionRects()[0].y));  // at the top
    p->onGesture(tapAt(p->addRect().center()), rig.nav);
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"to cite"});
    CHECK(rig.lib.recentAnnotationTags() == std::vector<std::string>{"to cite"});
    // Return keeps what is typed.
    rig.reader->onShow(rig.nav);
    rig.tap(rig.reader->annotationEditor()->bar().tags().addRect().center());
    TagPickerScreen* q = rig.picker();
    type(*q, rig.nav, "ideas");
    q->onKey({KeyEvent::Code::Return, ""}, rig.nav);
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"to cite", "ideas"});
    // Back leaves the annotation as it was.
    rig.reader->onShow(rig.nav);
    rig.tap(rig.reader->annotationEditor()->bar().tags().addRect().center());
    TagPickerScreen* r = rig.picker();
    type(*r, rig.nav, "nope");
    r->onKey({KeyEvent::Code::Escape, ""}, rig.nav);
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"to cite", "ideas"});
}

TEST_CASE("annotation tags: a tag picked after the bar closed still lands on its annotation") {
    ReaderRig rig;
    rig.tap(kOnBox);
    rig.tap(rig.reader->annotationEditor()->bar().tags().addRect().center());
    TagPickerScreen* p = rig.picker();
    rig.tap({600, 1300});  // the editor goes (as when the page changes under it)
    REQUIRE_FALSE(rig.reader->annotationEditor());
    type(*p, rig.nav, "kept");
    p->onKey({KeyEvent::Code::Return, ""}, rig.nav);
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"kept"});
}

TEST_CASE("annotation tags: + Tag on a new selection marks the new tag on it") {
    ReaderRig rig;
    rig.select({130, 510});
    rig.tap(rig.reader->annotationBar()->tags().addRect().center());
    TagPickerScreen* p = rig.picker();
    type(*p, rig.nav, "fresh");
    p->onKey({KeyEvent::Code::Return, ""}, rig.nav);
    rig.reader->onShow(rig.nav);
    REQUIRE(rig.reader->annotationBar());
    CHECK(rig.reader->annotationBar()->tags().chips().front() == "fresh");
    CHECK(rig.reader->annotationBar()->tags().isOn("fresh"));
    rig.tap(rig.reader->annotationBar()->buttonRect(1).center());  // Underline
    std::optional<AnnotationRecord> made;
    for (auto& a : rig.lib.annotationsFor("EP000003"))
        if (a.createdHere) made = a;
    REQUIRE(made);
    CHECK(made->tags == std::vector<std::string>{"fresh"});
}

// ---- the same row on an area and a sticky note ----------------------------------------------------

TEST_CASE("annotation tags: an area's bar has the same tag row") {
    ReaderRig rig;
    rig.remember({"figure"});
    AnnotationRecord a;
    a.key = "AREATAG1";
    a.parentKey = "EP000003";
    a.type = "image";
    a.color = "#5fb236";
    a.pageLabel = "0";
    a.version = 7;
    a.position = json{{"fake", 0}, {"rect", {300, 800, 400, 300}}}.dump();
    rig.lib.upsertAnnotation(a);
    rig.annotationsReady();
    REQUIRE(rig.waitFor([&] { return rig.reader->highlights().size() == 2; }));
    rig.tap({301, 900});
    REQUIRE(rig.reader->annotationEditor());
    CHECK(rig.reader->annotationEditor()->key() == "AREATAG1");
    const TagRow& row = rig.reader->annotationEditor()->bar().tags();
    CHECK(row.chips() == std::vector<std::string>{"figure"});
    rig.tap(row.chipRect(0).center());
    CHECK(rig.tags("AREATAG1") == std::vector<std::string>{"figure"});
    rig.tap(rig.reader->annotationEditor()->bar().tags().addRect().center());
    TagPickerScreen* p = rig.picker();
    type(*p, rig.nav, "chart");
    p->onKey({KeyEvent::Code::Return, ""}, rig.nav);
    CHECK(rig.tags("AREATAG1") == std::vector<std::string>{"figure", "chart"});
}

TEST_CASE("annotation tags: a sticky note's pop-up has the same tag row, with Edit and Delete") {
    ReaderRig rig(DocType::Pdf);
    rig.remember({"todo"});
    AnnotationRecord a;
    a.key = "NOTETAG1";
    a.parentKey = "EP000003";
    a.type = "note";
    a.color = "#ffd400";
    a.comment = "check this";
    a.pageLabel = "0";
    a.version = 3;
    a.position = json{{"fake", 0}, {"note", {820, 520}}}.dump();
    rig.lib.upsertAnnotation(a);
    rig.annotationsReady();
    REQUIRE(rig.waitFor([&] { return rig.reader->highlights().size() == 2; }));
    rig.tap({840, 540});
    const NotePopup* popup = rig.reader->notePopup();
    REQUIRE(popup);
    CHECK(popup->tags().chips() == std::vector<std::string>{"todo"});
    CHECK(inside(popup->tags().rect(), popup->rect()));
    CHECK(popup->tags().rect().y + popup->tags().rect().h <= popup->buttonRect(0).y);  // above Edit, Delete, Close
    const int pushes = rig.nav.syncFake.pushes;
    rig.tap(popup->tags().chipRect(0).center());
    REQUIRE(rig.reader->notePopup());  // stays open
    CHECK(rig.tags("NOTETAG1") == std::vector<std::string>{"todo"});
    CHECK(rig.reader->notePopup()->tags().isOn("todo"));
    CHECK(rig.nav.syncFake.pushes > pushes);
    rig.tap(rig.reader->notePopup()->tags().addRect().center());
    TagPickerScreen* p = rig.picker();
    type(*p, rig.nav, "urgent");
    p->onKey({KeyEvent::Code::Return, ""}, rig.nav);
    CHECK(rig.tags("NOTETAG1") == std::vector<std::string>{"todo", "urgent"});
    REQUIRE(rig.reader->notePopup());
    CHECK(rig.reader->notePopup()->tags().isOn("urgent"));
    CHECK(rig.reader->notePopup()->tags().chips().front() == "urgent");
    // Edit is still there: the comment, as before.
    rig.tap(rig.reader->notePopup()->buttonRect(0).center());
    REQUIRE(rig.reader->annotationEditor());
    CHECK(rig.reader->annotationEditor()->commenting());
}

TEST_CASE("annotation tags: book notes' editor has the tag row too") {
    ReaderRig rig;
    rig.remember({"method"});
    BookNotesScreen notes(*rig.lib.rowFor("IT000003"), nullptr);
    rig.nav.visible = &notes;
    notes.onShow(rig.nav);
    const Point row = notes.rowRects()[0].center();
    notes.onGesture(Gesture{GestureType::LongPress, row, row}, rig.nav);
    REQUIRE(notes.annotationEditor());
    notes.onGesture(tapAt(notes.annotationEditor()->bar().tags().chipRect(0).center()), rig.nav);
    CHECK(rig.tags("TAGME001") == std::vector<std::string>{"method"});
}

// ---- the lists -------------------------------------------------------------------------------------

namespace {

struct ListRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    ListRig() {
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 40);
        tag("N0000003", {"method"});
        tag("N1000003", {"method", "to-cite"});
        tag("N0000006", {"to-cite"});
        tag("N2000009", {"method"});
    }
    void tag(const std::string& key, std::vector<std::string> tags) {
        AnnotationRecord a = *lib.annotation(key);
        a.tags = std::move(tags);
        a.comment = "worth a second look";
        lib.upsertAnnotation(a);
    }
    TagPickerScreen* picker() {
        REQUIRE_FALSE(nav.pushed.empty());
        auto* p = dynamic_cast<TagPickerScreen*>(nav.pushed.back().get());
        REQUIRE(p);
        p->onShow(nav);
        return p;
    }
    static int notes(const NotesScreen& s) {
        int n = 0;
        for (const auto& l : s.page()) n += !l.header;
        return n;
    }
};

// Dark pixels in `r` of `c`.
int inkIn(const Canvas& c, Rect r) {
    const std::vector<uint8_t> g = grayOf(c);
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) n += g[static_cast<size_t>(y) * c.width() + x] < 200;
    return n;
}

}  // namespace

TEST_CASE("annotation tags: rows show the tags in a quiet line under the comment") {
    ListRig rig;
    AnnotationRecord a = *rig.lib.annotation("N1000003");
    CHECK(noteRowTags(a) == "#method · #to-cite");
    a.tags.clear();
    CHECK(noteRowTags(a).empty());
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    // Drawn: the line is there under the comment, and not without tags.
    Canvas with(1236, kNoteRowH), without(1236, kNoteRowH);
    const Rect row{kMargin, 0, 1236 - 2 * kMargin, kNoteRowH};
    drawNoteRow(with.cr(), row, *rig.lib.annotation("N1000003"));
    drawNoteRow(without.cr(), row, a);
    const Rect band{row.x + 34, 110, 700, 80};
    CHECK(inkIn(with, band) > inkIn(without, band) + 50);
    // The Notes tab and book notes list them so.
    NotesScreen tab;
    tab.onShow(rig.nav);
    tab.setTag("to-cite", rig.nav);
    bool shown = false;
    for (const auto& l : tab.page()) shown |= !l.header && noteRowTags(l.note.annotation) == "#method · #to-cite";
    CHECK(shown);
    BookNotesScreen book(*rig.lib.rowFor("IT000003"), nullptr);
    book.onShow(rig.nav);
    shown = false;
    for (const auto& n : book.shown()) shown |= noteRowTags(n) == "#method · #to-cite";
    CHECK(shown);
}

TEST_CASE("annotation tags: the Notes tab's Tag filter: the picker, the filter line with its count, and Clear") {
    ListRig rig;
    NotesScreen tab;
    tab.onShow(rig.nav);
    const int64_t all = tab.pager().total();
    tab.onGesture(tapAt(NotesScreen::tagButtonRect().center()), rig.nav);
    TagPickerScreen* p = rig.picker();
    CHECK_FALSE(p->isReading());
    REQUIRE(p->suggestions().size() == 2);  // the tags on annotations, most used first
    CHECK(p->suggestions()[0] == std::pair<std::string, int64_t>{"method", 3});
    CHECK(p->suggestions()[1] == std::pair<std::string, int64_t>{"to-cite", 2});
    type(*p, rig.nav, "meth");
    CHECK(p->newTag().empty());  // a filter adds no tags
    p->onGesture(tapAt(p->suggestionRects()[0].center()), rig.nav);
    CHECK(tab.tag() == "method");
    CHECK(tab.pager().total() == 3);
    CHECK(ListRig::notes(tab) == 3);
    for (const auto& l : tab.page())
        if (!l.header) CHECK(noteRowTags(l.note.annotation).find("#method") != std::string::npos);
    CHECK(tab.filterLine() == "tag: method · 3 notes");
    CHECK(tab.lineRects()[0].y >= NotesScreen::filterLineRect().y + NotesScreen::filterLineRect().h);  // the list under it
    // Notes' search still matches tags.
    CHECK(rig.lib.countAnnotations("cite") == 2);
    // Clear: every note again.
    tab.onGesture(tapAt(NotesScreen::clearRect().center()), rig.nav);
    CHECK(tab.tag().empty());
    CHECK(tab.pager().total() == all);
}

TEST_CASE("annotation tags: a book's notes have the Tag filter too, with its tags only") {
    ListRig rig;
    BookNotesScreen book(*rig.lib.rowFor("IT000003"), nullptr);
    rig.nav.visible = &book;
    book.onShow(rig.nav);
    REQUIRE(book.shown().size() == 3);
    book.onGesture(tapAt(BookNotesScreen::tagButtonRect().center()), rig.nav);
    TagPickerScreen* p = rig.picker();
    CHECK(p->isReading());  // one of the reader's panels
    REQUIRE(p->suggestions().size() == 2);  // this book's: N0 and N1 of item 3
    CHECK(p->suggestions()[0] == std::pair<std::string, int64_t>{"method", 2});
    CHECK(p->suggestions()[1] == std::pair<std::string, int64_t>{"to-cite", 1});
    p->onGesture(tapAt(p->suggestionRects()[1].center()), rig.nav);
    CHECK(book.tag() == "to-cite");
    REQUIRE(book.shown().size() == 1);
    CHECK(book.shown()[0].key == "N1000003");
    CHECK(book.filterLine() == "tag: to-cite · 1 note");
    REQUIRE(book.rowRects().size() == 1);
    CHECK(book.rowRects()[0].y >= kHeaderH + 50);  // under the filter line
    book.onGesture(tapAt(BookNotesScreen::clearRect().center()), rig.nav);
    CHECK(book.tag().empty());
    CHECK(book.shown().size() == 3);
}

// ---- fit -----------------------------------------------------------------------------------------

TEST_CASE("annotation tags: the tag row fits the bar on the PW5, KT4 and Scribe, chips at least 9 mm tall") {
    for (const Metrics* m : {&kPw5, &kKt4, &kScribe}) {
        ScopedMetrics s(*m);
        INFO(m->device);
        const Rect page = ReaderScreen::pageArea();
        TagRow longTags({"methodology-and-identification", "to-cite-in-chapter-three", "replication", "a"}, {});
        for (auto mode : {AnnotationBar::Mode::Create, AnnotationBar::Mode::Edit}) {
            // A line of text at the top, in the middle and at the bottom of the page: the bar never covers it.
            for (int y : {page.y + 40, page.y + page.h / 2, page.y + page.h - 120}) {
                const Rect line{100, y, page.w - 200, 50};
                AnnotationBar bar(mode, line, "#ffd400", mode == AnnotationBar::Mode::Create, longTags);
                CHECK(inside(bar.rect(), page));
                CHECK(bar.rect().intersected(line).empty());
                const TagRow& row = bar.tags();
                CHECK(inside(row.rect(), bar.rect()));
                std::vector<Rect> all;
                for (int i = 0; i < 4; ++i) all.push_back(row.chipRect(i));
                all.push_back(row.addRect());
                for (size_t i = 0; i < all.size(); ++i) {
                    CHECK(all[i].h >= 106);  // 9 mm at 300 ppi
                    CHECK(all[i].w >= 106);
                    CHECK(inside(all[i], row.rect()));
                    for (size_t j = i + 1; j < all.size(); ++j) CHECK(all[i].intersected(all[j]).empty());
                }
                // The row sits between the colours and the buttons.
                CHECK(row.rect().y >= bar.dotRect(0).y + bar.dotRect(0).h);
                CHECK(row.rect().y + row.rect().h <= bar.buttonRect(0).y);
                CHECK(bar.hit(row.chipRect(2).center()).action == AnnotationBar::Action::Tag);
                CHECK(bar.hit(row.chipRect(2).center()).colour == 2);
                CHECK(bar.hit(row.addRect().center()).action == AnnotationBar::Action::AddTag);
            }
        }
        // A sticky note's pop-up, in a corner too.
        AnnotationRecord n;
        n.type = "note";
        n.comment = "a note";
        for (Rect icon : {Rect{page.x + 10, page.y + 10, 40, 40}, Rect{page.x + page.w - 50, page.y + page.h - 50, 40, 40}}) {
            NotePopup p(n, icon, page, {"methodology-and-identification", "to-cite", "replication", "later"});
            CHECK(inside(p.rect(), page));
            CHECK(inside(p.tags().rect(), p.rect()));
            CHECK(p.tags().addRect().h >= 106);
            CHECK(inside(p.tags().addRect(), p.tags().rect()));
            for (int i = 0; i < 4; ++i) CHECK(inside(p.tags().chipRect(i), p.tags().rect()));
        }
        // The Notes tab's header: Tag beside the search, clear of the title.
        CHECK(NotesScreen::tagButtonRect().x > 400);
        CHECK(NotesScreen::tagButtonRect().x + NotesScreen::tagButtonRect().w <= NotesScreen::searchRect().x);
        CHECK(inside(BookNotesScreen::tagButtonRect(), {0, 0, m->width, kHeaderH}));
    }
}

// ---- goldens (PW5) -----------------------------------------------------------------------------------

TEST_CASE("annotation tags goldens: the bar with tag chips, the picker with Add, and Notes filtered by tag") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    {
        ReaderRig rig;
        rig.reader->turnTo(99);  // a light page
        REQUIRE(rig.waitFor([&] { return rig.reader->page() == 99; }));
        rig.remember({"later", "replication", "to-cite", "method"});
        rig.reader->onGesture(Gesture{GestureType::LongPress, {130, 210}, {130, 210}}, rig.nav);
        rig.reader->onGesture(Gesture{GestureType::DragStart, {200, 215}, {130, 210}}, rig.nav);
        rig.reader->onGesture(Gesture{GestureType::DragMove, {650, 330}, {130, 210}}, rig.nav);
        rig.reader->onGesture(Gesture{GestureType::DragEnd, {650, 330}, {130, 210}}, rig.nav);
        REQUIRE(rig.waitFor([&] { return rig.reader->annotationBar() && rig.reader->selection()->rects.size() == 3; }));
        rig.tap(rig.reader->annotationBar()->tags().chipRect(1).center());  // to-cite: filled
        Canvas c(1236, 1547);
        rig.reader->draw(c);
        checkGolden(c, "tests/golden/annotation-bar-tags.png");

        rig.tap(rig.reader->annotationBar()->tags().addRect().center());
        TagPickerScreen* p = rig.picker();
        type(*p, rig.nav, "re");
        p->draw(c);
        checkGolden(c, "tests/golden/tag-picker-add.png");
    }
    ListRig rig;
    NotesScreen tab;
    tab.onShow(rig.nav);
    tab.setTag("method", rig.nav);
    Canvas c(1236, 1547);
    tab.draw(c);
    drawTabBar(c.cr(), {0, 1547 - kTabBarH, 1236, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 3);
    checkGolden(c, "tests/golden/notes-tag-filter.png");
}
