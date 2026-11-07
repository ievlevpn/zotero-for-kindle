// Plan 6 final-review findings, each pinned by a test.
#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "screens/annotation_bar.h"
#include "screens/home.h"
#include "screens/notes.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

// One highlight in each of `books` books, newest first.
void oneHighlightPerBook(Library& lib, int books) {
    for (int i = 0; i < books; ++i) {
        ItemRecord item;
        item.key = fixtureKey("NB", i);
        item.itemType = "book";
        item.title = "Book number " + std::to_string(i);
        lib.upsertItem(item);
        AttachmentRecord att;
        att.key = fixtureKey("NA", i);
        att.parentKey = item.key;
        att.contentType = "application/epub+zip";
        lib.upsertAttachment(att);
        AnnotationRecord a;
        a.key = fixtureKey("NN", i);
        a.parentKey = att.key;
        a.type = "highlight";
        a.text = "a highlight in book " + std::to_string(i);
        a.version = 1;
        char when[32];
        std::snprintf(when, sizeof when, "2026-11-07T10:%02d:00Z", 59 - i);
        a.dateModified = when;
        lib.upsertAnnotation(a);
    }
}

}  // namespace

TEST_CASE("review 1: on the Scribe, notes from many different books stay above the page indicator") {
    ScopedMetrics s(kScribe);
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    oneHighlightPerBook(lib, 12);
    NotesScreen notes;
    notes.onShow(nav);
    const int indicatorTop = 2379 - kTabBarH - kPageIndicatorH;
    REQUIRE_FALSE(notes.lineRects().empty());
    for (const auto& r : notes.lineRects()) CHECK(r.y + r.h <= indicatorTop);
    int shown = 0;
    for (const auto& l : notes.page()) shown += !l.header;
    CHECK(shown >= 5);  // still more than the PW5's 4
}

TEST_CASE("review 1: on the PW5 the Notes page still holds 4 notes") {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    oneHighlightPerBook(lib, 12);
    NotesScreen notes;
    notes.onShow(nav);
    int shown = 0;
    for (const auto& l : notes.page()) shown += !l.header;
    CHECK(shown == 4);
    for (const auto& r : notes.lineRects()) CHECK(r.y + r.h <= 1547 - kTabBarH - kPageIndicatorH);
}

TEST_CASE("review 2: the comment box sits above the keyboard lipc reports, wherever it is") {
    ScopedMetrics s(kScribe);
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    nav.keyboardFake.fixed = {0, 1000, 1860, 1379};  // a taller keyboard than the profile's
    AnnotationRecord a;
    a.key = "ANNO0001";
    a.color = "#ffd400";
    lib.upsertAnnotation(a);
    AnnotationEditor editor(a, {100, 300, 600, 50});
    editor.onTap(editor.bar().buttonRect(0).center(), nav);  // Comment
    REQUIRE(editor.commenting());
    CHECK(editor.rect().y + editor.rect().h <= 1000);
    CHECK(editor.rect().y >= 0);
}

TEST_CASE("review 3: very long titles stay inside the card and rows on the Scribe (golden)") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    ScopedMetrics s(kScribe);
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    const std::string latin(300, 'x');
    const char* titles[] = {"On the extraordinarily long and winding titles of nineteenth-century treatises concerning the "
                            "theory of value, wages, rent, profits and the natural price of labour in a commercial society",
                            "Философия общего дела: статьи, мысли и письма Николая Фёдоровича Фёдорова, изданные посмертно "
                            "В. А. Кожевниковым и Н. П. Петерсоном, том первый и второй вместе"};
    for (int i = 0; i < 9; ++i) {
        ItemRecord r;
        r.key = fixtureKey("LT", i);
        r.title = i % 3 == 2 ? latin : titles[i % 2];
        for (int c = 0; c < 6; ++c) r.creators.push_back({"A.", "Verylongsurnameforlayout" + std::to_string(c), "", "author"});
        r.year = "1903";
        r.dateAdded = "2024-01-0" + std::to_string(i + 1);
        r.json = "{}";
        lib.upsertItem(r);
        lib.upsertAttachment({fixtureKey("LA", i), r.key, "application/epub+zip", "b.epub", "", "imported_file", 0, "",
                              AttachmentRecord::State::Remote});
    }
    lib.markOpened("LT000001", "LA000001", 1'762'500'000'000 - 3600 * 1000LL);
    SyncStatus status;
    HomeScreen home(status, HomeActions{}, [] { return int64_t{1'762'500'000'000}; });
    home.onShow(nav);
    Canvas c(1860, 2379);
    home.draw(c);
    drawTabBar(c.cr(), {0, 2379 - kTabBarH, 1860, kTabBarH}, {"Home", "Library", "Collections", "Notes"}, 0);
    checkGolden(c, "tests/golden/home-stress-real-scribe.png");
}
