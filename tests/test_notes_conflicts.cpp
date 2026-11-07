// Plan 5 Task 9: Notes shows each annotation's sync state; a conflict can be re-applied or
// discarded (after asking).

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "golden.h"
#include "screens/notes.h"
#include "store/db.h"
#include "ui/text.h"
#include "ui/widgets.h"

using namespace zk;
using namespace zk::test;

namespace {

struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    NotesScreen notes;
    Rig() {
        addFontDir("assets/fonts-device");
        ItemRecord item;
        item.key = "BOOK0001";
        item.itemType = "book";
        item.title = "A book with conflicts";
        lib.upsertItem(item);
        AttachmentRecord att;
        att.key = "EPUB0001";
        att.parentKey = "BOOK0001";
        att.contentType = "application/epub+zip";
        lib.upsertAttachment(att);
    }
    AnnotationRecord make(const std::string& key, const std::string& text, const std::string& when) {
        AnnotationRecord a;
        a.key = key;
        a.parentKey = "EPUB0001";
        a.type = "highlight";
        a.text = text;
        a.color = "#ffd400";
        a.dateModified = when;
        return a;
    }
    void show() {
        nav.visible = &notes;
        notes.onShow(nav);
    }
    // The note line for `key` on the current page, and its rect.
    std::optional<Rect> rectOf(const std::string& key) {
        auto rects = notes.lineRects();
        for (size_t i = 0; i < notes.page().size(); ++i)
            if (!notes.page()[i].header && notes.page()[i].note.annotation.key == key) return rects[i];
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("notes marks: each sync state has its words") {
    AnnotationRecord a;
    CHECK(noteSyncMark(a).empty());  // in Zotero, nothing to say
    a.dirty = true;
    CHECK(noteSyncMark(a) == "not synced yet");
    a.localOnly = true;
    CHECK(noteSyncMark(a) == "on the Kindle only");
    a.localOnly = false;
    a.dirty = false;
    a.conflict = true;
    CHECK(noteSyncMark(a) == "conflict — Zotero's version kept");
}

TEST_CASE("notes conflicts: Re-apply makes the Kindle's version a change to send and removes the conflict") {
    Rig rig;
    AnnotationRecord c = rig.make("CONFLIC1", "my version", "2026-11-07T10:00:00Z");
    c.conflict = true;
    c.createdHere = true;
    rig.lib.upsertAnnotation(c);
    rig.show();
    auto row = rig.rectOf("CONFLIC1");
    REQUIRE(row);
    rig.notes.onGesture(tapAt(noteRowButtonRect(*row, 0).center()), rig.nav);  // Re-apply
    CHECK(rig.lib.conflicts().empty());
    auto dirty = rig.lib.dirtyAnnotations();
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0].text == "my version");
    CHECK_FALSE(dirty[0].conflict);
    CHECK(rig.nav.pushed.empty());  // the button, not the row's "open the book"
}

TEST_CASE("notes conflicts: Discard asks first, then removes the conflict; Cancel keeps it") {
    Rig rig;
    AnnotationRecord c = rig.make("CONFLIC1", "my version", "2026-11-07T10:00:00Z");
    c.conflict = true;
    rig.lib.upsertAnnotation(c);
    rig.show();
    auto row = rig.rectOf("CONFLIC1");
    REQUIRE(row);
    rig.notes.onGesture(tapAt(noteRowButtonRect(*row, 1).center()), rig.nav);  // Discard
    CHECK(rig.notes.confirmingDiscard());
    CHECK(rig.lib.conflicts().size() == 1);
    rig.notes.onGesture(tapAt(NotesScreen::confirmRect(1).center()), rig.nav);  // Cancel
    CHECK_FALSE(rig.notes.confirmingDiscard());
    CHECK(rig.lib.conflicts().size() == 1);
    rig.notes.onGesture(tapAt(noteRowButtonRect(*row, 1).center()), rig.nav);
    rig.notes.onGesture(tapAt(NotesScreen::confirmRect(0).center()), rig.nav);  // Discard
    CHECK(rig.lib.conflicts().empty());
    CHECK_FALSE(rig.rectOf("CONFLIC1"));
}

TEST_CASE("notes: deleted-here annotations are hidden; other states are listed") {
    Rig rig;
    AnnotationRecord gone = rig.make("GONE0001", "deleted here", "2026-11-07T09:00:00Z");
    gone.version = 4;
    rig.lib.upsertAnnotation(gone);
    rig.lib.deleteAnnotation("GONE0001");
    rig.lib.createAnnotation(rig.make("", "made here", ""));
    rig.show();
    CHECK_FALSE(rig.rectOf("GONE0001"));
    int notes = 0;
    for (const auto& l : rig.notes.page()) notes += !l.header;
    CHECK(notes == 1);
}

TEST_CASE("notes with sync marks and a conflict match the reviewed golden") {
    Rig rig;
    AnnotationRecord synced = rig.make("SYNCED01", "It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife.", "2026-11-07T08:00:00Z");
    synced.version = 3;
    rig.lib.upsertAnnotation(synced);
    AnnotationRecord unsent = rig.make("UNSENT01", "However little known the feelings or views of such a man may be on his first entering a neighbourhood", "2026-11-07T09:00:00Z");
    unsent.dirty = true;
    unsent.createdHere = true;
    rig.lib.upsertAnnotation(unsent);
    AnnotationRecord local = rig.make("LOCAL001", "this truth is so well fixed in the minds of the surrounding families", "2026-11-07T09:30:00Z");
    local.dirty = local.localOnly = local.createdHere = true;
    rig.lib.upsertAnnotation(local);
    AnnotationRecord conflict = rig.make("CONFLIC1", "that he is considered the rightful property of some one or other of their daughters.", "2026-11-07T10:00:00Z");
    conflict.comment = "my comment from the Kindle";
    conflict.conflict = conflict.createdHere = true;
    rig.lib.upsertAnnotation(conflict);
    rig.show();
    Canvas c(1236, 1547);
    rig.notes.draw(c);
    checkGolden(c, "tests/golden/notes-conflicts.png");
}
