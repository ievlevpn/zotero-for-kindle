// Long-press on an item: the details sheet (title, people, collections, the file, the abstract)
// with Open / Remove from Kindle / More…, and the full details page.
#include <doctest/doctest.h>

#include "fake_nav.h"
#include "golden.h"
#include "screens/home.h"
#include "screens/item_details.h"
#include "screens/item_list.h"
#include "screens/item_sheet.h"
#include "store/db.h"
#include "store/library.h"
#include "screens/sync_status.h"
#include "ui/canvas.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

bool fonts() {
    if (!fs::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

// Pride and Prejudice, downloaded, read to 13%, with two highlights, in Classics › Novels;
// and a paper with no file.
struct Rig {
    Db db = Db::open(":memory:");
    Library lib{db};
    FakeNav nav{lib};
    fs::path files;
    Rig() {
        char tmpl[] = "/tmp/zk-details-XXXXXX";
        files = mkdtemp(tmpl);
        auto tx = lib.begin();
        lib.upsertCollection({"COLCLASS", "Classics", "", 1});
        lib.upsertCollection({"COLNOVEL", "Novels", "COLCLASS", 1});
        ItemRecord r;
        r.key = "PRIDE001";
        r.version = 1;
        r.itemType = "book";
        r.title = "Pride and Prejudice";
        r.creators = {{"Jane", "Austen", "", "author"}, {"Vivien", "Jones", "", "editor"}};
        r.date = r.year = "1813";
        r.publication = "Penguin Classics";
        r.collections = {"COLNOVEL"};
        r.tags = {"to-read", "novels"};
        r.dateAdded = r.dateModified = "2026-10-01T12:00:00Z";
        r.json = R"({"abstractNote":"It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife.","publisher":"Penguin Classics"})";
        lib.upsertItem(r);
        AttachmentRecord a;
        a.key = "PRIDEEPB";
        a.parentKey = "PRIDE001";
        a.contentType = "application/epub+zip";
        a.filename = "pride.epub";
        a.linkMode = "imported_file";
        lib.upsertAttachment(a);
        for (int i = 0; i < 2; ++i) {
            AnnotationRecord an;
            an.key = "HL00000" + std::to_string(i);
            an.parentKey = "PRIDEEPB";
            an.type = "highlight";
            an.text = "words";
            an.sortIndex = "00001|0000000" + std::to_string(i);
            an.version = 1;
            lib.upsertAnnotation(an);
        }
        ItemRecord paper;
        paper.key = "PAPER001";
        paper.version = 1;
        paper.itemType = "journalArticle";
        paper.title = "The Nature of the Firm";
        paper.creators = {{"Ronald", "Coase", "", "author"}};
        paper.date = paper.year = "1937";
        paper.publication = "Economica";
        paper.dateAdded = paper.dateModified = "2026-10-02T12:00:00Z";
        paper.json = "{}";
        lib.upsertItem(paper);
        tx.commit();
        const fs::path book = files / "PRIDEEPB" / "pride.epub";
        fs::create_directories(book.parent_path());
        std::ofstream(book) << std::string(820'000, 'x');
        lib.setFileState("PRIDEEPB", AttachmentRecord::State::Local, book.string());
        lib.markOpened("PRIDE001", "PRIDEEPB", 1000);
        lib.saveReading("PRIDE001", "loc", 0.13);
    }
    ~Rig() { fs::remove_all(files); }
    ItemRow row(const std::string& key) { return *lib.rowFor(key); }
};

}  // namespace

TEST_CASE("item details: an item's people, collections, file, highlights and abstract") {
    Rig rig;
    auto d = rig.lib.details("PRIDE001");
    REQUIRE(d);
    CHECK(d->item.title == "Pride and Prejudice");
    REQUIRE(d->item.creators.size() == 2);
    CHECK(d->abstract.rfind("It is a truth universally acknowledged", 0) == 0);
    REQUIRE(d->collectionPaths.size() == 1);
    CHECK(d->collectionPaths[0] == "Classics › Novels");
    REQUIRE(d->files.size() == 1);
    CHECK(d->files[0].attachment.key == "PRIDEEPB");
    CHECK(d->files[0].onKindle);
    CHECK(d->files[0].bytes == 820'000);
    CHECK(d->files[0].highlights == 2);
    CHECK(d->progress == doctest::Approx(0.13));
    auto paper = rig.lib.details("PAPER001");
    REQUIRE(paper);
    CHECK(paper->files.empty());
    CHECK(paper->abstract.empty());
    CHECK(paper->progress < 0);
    CHECK_FALSE(rig.lib.details("NOPE0000"));
}

TEST_CASE("item details: removing a download deletes the file and keeps everything else") {
    Rig rig;
    const fs::path book = rig.files / "PRIDEEPB" / "pride.epub";
    rig.lib.removeDownload("PRIDEEPB");
    CHECK_FALSE(fs::exists(book));
    CHECK_FALSE(fs::exists(book.parent_path()));
    auto a = rig.lib.attachment("PRIDEEPB");
    REQUIRE(a);
    CHECK(a->state == AttachmentRecord::State::Remote);
    CHECK(a->localPath.empty());
    CHECK(rig.lib.annotationsFor("PRIDEEPB").size() == 2);
    CHECK(rig.lib.readingState("PRIDE001")->progress == doctest::Approx(0.13));
    CHECK(rig.lib.rowFor("PRIDE001"));  // still in the library
    rig.lib.removeDownload("PRIDEEPB");  // twice is harmless
}

TEST_CASE("item sheet: Open, Remove from Kindle (two taps) and More…") {
    Rig rig;
    std::vector<std::string> opened;
    ItemSheet sheet(rig.row("PRIDE001"), *rig.lib.details("PRIDE001"));
    const Rect area{0, 0, metrics().width, metrics().height};
    CHECK(sheet.onGesture(tapAt(sheet.openRect(area).center()), area, rig.nav) == ItemSheet::Action::Open);
    // Remove asks once, then removes.
    CHECK(sheet.onGesture(tapAt(sheet.removeRect(area).center()), area, rig.nav) == ItemSheet::Action::None);
    CHECK(sheet.confirmingRemove());
    CHECK(fs::exists(rig.files / "PRIDEEPB" / "pride.epub"));
    CHECK(sheet.onGesture(tapAt(sheet.removeRect(area).center()), area, rig.nav) == ItemSheet::Action::Removed);
    CHECK_FALSE(fs::exists(rig.files / "PRIDEEPB" / "pride.epub"));
    CHECK_FALSE(sheet.details().files[0].onKindle);  // the sheet now says "in Zotero"
    CHECK(sheet.onGesture(tapAt(sheet.moreRect(area).center()), area, rig.nav) == ItemSheet::Action::More);
    CHECK(sheet.onGesture(tapAt({area.w / 2, 20}), area, rig.nav) == ItemSheet::Action::Close);  // above the sheet
}

TEST_CASE("item list: a long-press opens the sheet; Open opens the item; above it closes") {
    Rig rig;
    ItemList list = ItemList::forQuery(ListQuery{});
    list.refresh(rig.lib);
    const Rect area{0, 200, metrics().width, 1100};
    std::vector<std::string> opened;
    auto open = [&](const ItemRow& r) { opened.push_back(r.key); };
    REQUIRE_FALSE(list.rows().empty());
    size_t at = 0;  // the book with a file (the paper has none, so its Open is greyed out)
    while (at < list.rows().size() && list.rows()[at].key != "PRIDE001") ++at;
    REQUIRE(at < list.rows().size());
    const Rect first = list.rowRects(area)[at];
    const std::string key = list.rows()[at].key;
    CHECK(list.onGesture(Gesture{GestureType::LongPress, first.center(), first.center()}, area, rig.nav, open));
    REQUIRE(list.sheet());
    CHECK(opened.empty());
    list.onGesture(tapAt(list.sheet()->openRect(area).center()), area, rig.nav, open);
    CHECK(opened == std::vector<std::string>{key});
    CHECK_FALSE(list.sheet());  // closed when the item opens
    list.onGesture(Gesture{GestureType::LongPress, first.center(), first.center()}, area, rig.nav, open);
    REQUIRE(list.sheet());
    list.onGesture(tapAt({area.w / 2, area.y + 10}), area, rig.nav, open);
    CHECK_FALSE(list.sheet());
    CHECK(opened.size() == 1);  // the tap that closed it opened nothing
}

TEST_CASE("item list: More… opens the details page") {
    Rig rig;
    ItemList list = ItemList::forQuery(ListQuery{});
    list.refresh(rig.lib);
    const Rect area{0, 200, metrics().width, 1100};
    const Rect first = list.rowRects(area)[0];
    list.onGesture(Gesture{GestureType::LongPress, first.center(), first.center()}, area, rig.nav, nullptr);
    REQUIRE(list.sheet());
    list.onGesture(tapAt(list.sheet()->moreRect(area).center()), area, rig.nav, nullptr);
    REQUIRE(rig.nav.pushed.size() == 1);
    CHECK(dynamic_cast<ItemDetailsScreen*>(rig.nav.pushed[0].get()));
}

TEST_CASE("home: a long-press on the Continue reading card opens the sheet") {
    Rig rig;
    SyncStatus status;
    std::vector<std::string> opened;
    HomeActions actions;
    actions.openItem = [&](const ItemRow& r) { opened.push_back(r.key); };
    HomeScreen home(status, actions, [] { return int64_t{2000}; });
    home.onShow(rig.nav);
    const Point p = HomeScreen::cardRect().center();
    home.onGesture(Gesture{GestureType::LongPress, p, p}, rig.nav);
    REQUIRE(home.sheet());
    CHECK(home.sheet()->row().key == "PRIDE001");
    CHECK(opened.empty());
}

TEST_CASE("item details: the sheet and the page match the reviewed goldens") {
    if (!fonts()) return;
    for (const Metrics& m : {kPw5, kScribe}) {
        ScopedMetrics sm(m);
        Rig rig;
        SyncStatus status;
        HomeScreen home(status, HomeActions{}, [] { return int64_t{2000}; });
        home.onShow(rig.nav);
        const Point p = HomeScreen::cardRect().center();
        home.onGesture(Gesture{GestureType::LongPress, p, p}, rig.nav);
        Canvas c(m.width, m.height);
        home.draw(c);
        checkGolden(c, m.device == kPw5.device ? "tests/golden/item-sheet.png" : "tests/golden/item-sheet-scribe.png");
        ItemDetailsScreen page(rig.row("PRIDE001"), nullptr);
        page.onShow(rig.nav);
        Canvas c2(m.width, m.height);
        page.draw(c2);
        checkGolden(c2, m.device == kPw5.device ? "tests/golden/item-details.png" : "tests/golden/item-details-scribe.png");
    }
}
