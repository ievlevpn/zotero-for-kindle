// Items with more than one readable file: "Open which file?", "2 files" in rows, Open per file in
// the item's details, Continue reading's exact file, each file's own place.

#include <doctest/doctest.h>

#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "library_nav_fit.h"
#include "screens/file_choice.h"
#include "screens/home.h"
#include "screens/item_details.h"
#include "screens/library.h"
#include "screens/notes.h"
#include "screens/open_flow.h"
#include "screens/sync_status.h"
#include "store/db.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "ui/widgets.h"

#include <chrono>
#include <filesystem>

using namespace zk;
using namespace zk::test;

namespace {

constexpr int64_t kWall = 1'762'500'000'000;

// IT000000 ("Recursive Macroeconomic Theory") has its EPUB (EP000000) and a PDF too.
struct Rig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    OpenFlow flow{[] { return kWall; }};
    Rig() {
        fillFixtureLibrary(lib, 40);
        AttachmentRecord pdf{"PD900000", "IT000000", "application/pdf", "ljungqvist-sargent.pdf", "", "imported_file", 0, "",
                             AttachmentRecord::State::Remote, "Full Text PDF"};
        lib.upsertAttachment(pdf);
    }
    ItemRow row() { return *lib.rowFor("IT000000"); }
};

bool fonts() {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

// Turns the Library's list to the page showing that item; its row's index there.
int showItem(LibraryScreen& screen, Library& lib, const std::string& key) {
    for (int page = 0; screen.list().goToPage(page, lib); ++page)
        for (size_t i = 0; i < screen.list().rows().size(); ++i)
            if (screen.list().rows()[i].key == key) return static_cast<int>(i);
    return -1;
}

FileChoiceScreen* choice(FakeNav& nav) { return nav.pushed.empty() ? nullptr : dynamic_cast<FileChoiceScreen*>(nav.pushed.back().get()); }

}  // namespace

TEST_CASE("files: an item with one file opens directly") {
    Rig rig;
    ItemRow one = *rig.lib.rowFor("IT000003");
    CHECK(one.files == 1);
    rig.flow.open(one, rig.nav);
    CHECK(rig.nav.pushed.empty());
    REQUIRE(rig.nav.syncFake.opened.size() == 1);
    CHECK(rig.nav.syncFake.opened[0] == "IT000003/EP000003");
}

TEST_CASE("files: several files ask which, the one read last first and marked") {
    Rig rig;
    CHECK(rig.row().files == 2);
    rig.flow.open(rig.row(), rig.nav);
    CHECK(rig.nav.syncFake.opened.empty());
    auto* sheet = choice(rig.nav);
    REQUIRE(sheet);
    REQUIRE(sheet->files().size() == 2);
    CHECK(sheet->files()[0].attachment.key == "EP000000");  // never read: EPUB first
    CHECK(sheet->files()[1].title == "Full Text PDF");
    CHECK_FALSE(sheet->files()[0].lastRead);
    CHECK(FileChoiceScreen::fileMeta(sheet->files()[0]) == "EPUB");

    rig.lib.markOpened("IT000000", "PD900000", kWall);
    rig.lib.saveReading("IT000000", "PD900000", "pos:7", 0.42);
    rig.lib.setFileState("PD900000", AttachmentRecord::State::Local, "/tmp/x.pdf");
    auto files = rig.lib.fileChoices("IT000000");
    CHECK(files[0].attachment.key == "PD900000");
    CHECK(files[0].lastRead);
    CHECK(FileChoiceScreen::fileMeta(files[0]) == "PDF · On Kindle · 42%");
    CHECK(files[1].title == "book.epub");  // no title: the filename
}

TEST_CASE("files: a choice opens that file, in the item's row, and becomes the one read last") {
    Rig rig;
    rig.lib.markOpened("IT000000", "EP000000", kWall - 1000);
    LibraryScreen screen([&](const ItemRow& r) { rig.flow.open(r, rig.nav); }, rig.flow.statusFn());
    rig.nav.visible = &screen;
    screen.onShow(rig.nav);
    LibraryView v;
    v.sort = SortBy::Title;
    screen.setView(v, rig.nav);
    const int index = showItem(screen, rig.lib, "IT000000");
    REQUIRE(index >= 0);
    screen.onGesture(tapAt(screen.list().rowRects(LibraryScreen::listArea())[static_cast<size_t>(index)].center()), rig.nav);
    auto* sheet = choice(rig.nav);
    REQUIRE(sheet);
    CHECK(sheet->files()[0].attachment.key == "EP000000");  // read last
    CHECK(sheet->files()[0].lastRead);
    sheet->onGesture(tapAt(sheet->fileRects()[1].center()), rig.nav);  // the PDF
    CHECK(rig.nav.pops == 1);  // the sheet closed first
    REQUIRE(rig.nav.syncFake.opened.size() == 1);
    CHECK(rig.nav.syncFake.opened[0] == "IT000000/PD900000");
    // Its download shows in the item's row (whose own file is the EPUB).
    CHECK(rig.flow.statusFor("EP000000") == "Downloading…");
    SyncEvent p;
    p.kind = SyncEvent::Kind::DownloadProgress;
    p.itemKey = "IT000000";
    p.attachmentKey = "PD900000";
    p.done = 1;
    p.total = 2;
    rig.flow.onSyncEvent(p, rig.nav);
    rig.nav.damages.clear();
    screen.onSyncEvent(p, rig.nav);
    CHECK(rig.flow.statusFor("EP000000") == "Downloading 50%");
    REQUIRE(rig.nav.damages.size() == 1);
    CHECK(rig.nav.damages[0].rect == screen.list().rowRects(LibraryScreen::listArea())[static_cast<size_t>(index)]);
    // Done: the PDF opens, and is the one read last.
    rig.lib.setFileState("PD900000", AttachmentRecord::State::Local, "/tmp/x.pdf");
    SyncEvent done = p;
    done.kind = SyncEvent::Kind::DownloadFinished;
    rig.flow.onSyncEvent(done, rig.nav);
    CHECK(rig.lib.readingState("IT000000")->attachmentKey == "PD900000");
    CHECK(rig.lib.fileChoices("IT000000")[0].attachment.key == "PD900000");
}

TEST_CASE("files: each file keeps its own place; the item's is its last file's") {
    Rig rig;
    rig.lib.markOpened("IT000000", "EP000000", kWall);
    rig.lib.saveReading("IT000000", "EP000000", "epub-place", 0.3);
    rig.lib.markOpened("IT000000", "PD900000", kWall + 1);
    CHECK(rig.lib.readingLocation("PD900000").empty());  // a file never read starts at its beginning
    CHECK(rig.lib.readingState("IT000000")->progress == doctest::Approx(0));
    rig.lib.saveReading("IT000000", "PD900000", "pdf-place", 0.8);
    rig.lib.markOpened("IT000000", "EP000000", kWall + 2);
    CHECK(rig.lib.readingLocation("EP000000") == "epub-place");
    CHECK(rig.lib.readingLocation("PD900000") == "pdf-place");
    CHECK(rig.lib.readingState("IT000000")->progress == doctest::Approx(0.3));
    CHECK(rig.lib.rowFor("IT000000")->progress == doctest::Approx(0.3));
}

TEST_CASE("files: rows say \"2 files\" next to what is there") {
    Rig rig;
    ItemRow r = rig.row();
    CHECK(itemRowMeta(r) == "2 files · new");
    r.progress = 0.45;
    CHECK(itemRowMeta(r) == "2 files · 45%");
    r.read = true;
    CHECK(itemRowMeta(r) == "2 files");
    CHECK(itemRowMeta(*rig.lib.rowFor("IT000003")) == "new");
    ListQuery q;
    q.sort = SortBy::Title;
    q.limit = 100;
    for (const auto& row : rig.lib.rows(q)) CHECK(row.files == (row.key == "IT000000" ? 2 : row.badge.empty() || row.key == "LONE0001" ? 0 : 1));
}

TEST_CASE("files: Downloaded counts an item with any of its files on the Kindle") {
    Rig rig;
    ListQuery q;
    q.downloaded = true;
    CHECK(rig.lib.countRows(q) == 0);
    rig.lib.setFileState("PD900000", AttachmentRecord::State::Local, "/tmp/x.pdf");  // not the row's own file
    REQUIRE(rig.lib.countRows(q) == 1);
    CHECK(rig.lib.rows(q)[0].key == "IT000000");
    q.downloaded = false;
    q.type = TypeFilter::WithFile;
    CHECK(rig.lib.countRows(q) == rig.lib.countRows(ListQuery{.type = TypeFilter::WithFile}));
}

TEST_CASE("files: the details page has Open for each file, opening that one") {
    Rig rig;
    std::vector<ItemRow> opened;
    ItemDetailsScreen details(rig.row(), [&](const ItemRow& r) { opened.push_back(r); });
    details.onShow(rig.nav);
    Canvas c(1236, 1547);
    details.draw(c);  // lays the buttons out
    REQUIRE(details.fileOpenRects().size() == 2);
    details.onGesture(tapAt(details.fileOpenRects()[1].center()), rig.nav);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].attachmentKey == "PD900000");
    CHECK(opened[0].exactFile);
    rig.flow.open(opened[0], rig.nav);  // straight to the file: no question
    CHECK(rig.nav.pushed.empty());
    CHECK(rig.nav.syncFake.opened.back() == "IT000000/PD900000");
}

TEST_CASE("files: Continue reading reopens the very file read last") {
    Rig rig;
    rig.lib.markOpened("IT000000", "PD900000", kWall);
    SyncStatus status;
    std::vector<ItemRow> opened;
    HomeScreen home{status, HomeActions{nullptr, nullptr, [&](const ItemRow& r) { opened.push_back(r); }, nullptr},
                    [] { return kWall; }};
    home.onShow(rig.nav);
    home.onGesture(tapAt(HomeScreen::cardRect().center()), rig.nav);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].attachmentKey == "PD900000");
    CHECK(opened[0].badge == "PDF");
    rig.flow.open(opened[0], rig.nav);
    CHECK(rig.nav.pushed.empty());
    CHECK(rig.nav.syncFake.opened.back() == "IT000000/PD900000");
}

TEST_CASE("files: a highlight in Notes opens its own file") {
    Rig rig;
    AnnotationRecord a;
    a.key = "PDFNOTE1";
    a.parentKey = "PD900000";
    a.type = "highlight";
    a.text = "in the PDF";
    a.dateModified = "2030-01-01T00:00:00Z";  // newest: first
    rig.lib.upsertAnnotation(a);
    std::vector<ItemRow> opened;
    NotesScreen notes([&](const ItemRow& r, std::optional<AnnotationRecord>) { opened.push_back(r); });
    notes.onShow(rig.nav);
    notes.onGesture(tapAt(notes.lineRects()[1].center()), rig.nav);
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].attachmentKey == "PD900000");
}

TEST_CASE("files: a kept collection covers every readable file of its items") {
    Rig rig;  // IT000000 is in Economics
    std::vector<std::string> files;
    for (const auto& r : rig.lib.collectionFiles("COLLAAAA"))
        if (r.key == "IT000000") files.push_back(r.attachmentKey);
    CHECK(files == std::vector<std::string>{"EP000000", "PD900000"});
    CHECK(rig.lib.filesOfManyFileItems() == std::vector<std::string>{"EP000000", "PD900000"});
}

TEST_CASE("files: the sheet and a \"2 files\" row match the reviewed goldens") {
    if (!fonts()) return;
    Rig rig;
    rig.lib.markOpened("IT000000", "PD900000", kWall);
    rig.lib.saveReading("IT000000", "PD900000", "pos:7", 0.42);
    rig.lib.setFileState("PD900000", AttachmentRecord::State::Local, "/tmp/x.pdf");
    LibraryScreen screen([&](const ItemRow& r) { rig.flow.open(r, rig.nav); });
    rig.nav.visible = &screen;
    screen.onShow(rig.nav);
    LibraryView v;
    v.sort = SortBy::Title;
    v.type = TypeFilter::Any;
    screen.setView(v, rig.nav);
    REQUIRE(showItem(screen, rig.lib, "IT000000") >= 0);
    Canvas c(1236, 1547);
    screen.draw(c);
    checkGolden(c, "tests/golden/library-two-files.png");
    rig.flow.open(rig.row(), rig.nav);
    auto* sheet = choice(rig.nav);
    REQUIRE(sheet);
    sheet->draw(c);
    checkGolden(c, "tests/golden/open-which-file.png");
}

TEST_CASE("files: the sheet and a \"2 files\" row fit on the KT4 and the Scribe") {
    Rig rig;
    addFontDir("assets/fonts-device");
    for (const Metrics* m : {&kKt4, &kScribe}) {
        ScopedMetrics s(*m);
        CAPTURE(m->width);
        std::vector<FileChoice> files = rig.lib.fileChoices("IT000000");
        files.push_back(files[0]);  // three, the longest title
        files.back().title = std::string(200, 'W');
        FileChoiceScreen sheet(rig.row(), files, nullptr, {});
        const Rect window{0, 0, metrics().width, metrics().height - kTabBarH};
        CHECK(within(sheet.sheetRect(), window));
        REQUIRE(sheet.fileRects().size() == 3);
        for (const Rect& r : sheet.fileRects()) CHECK(within(r, sheet.sheetRect()));
        Canvas c(metrics().width, metrics().height);
        sheet.draw(c);
        ItemRow r = rig.row();
        r.progress = 0.45;
        const Rect row{kMargin, 300, metrics().width - 2 * kMargin, kRowH};
        drawItemRow(c.cr(), row, r, "");
        const int metaW = measureText(itemRowMeta(r), {theme::kSans, 6.5, 400, false, theme::kTertiary}, row.w, 1).width;
        CHECK(metaW < row.w / 3);  // the byline keeps most of the line
    }
}

TEST_CASE("files: the real library (a copy: ZK_REAL_LIBRARY)") {
    const char* path = std::getenv("ZK_REAL_LIBRARY");
    if (!path) return;
    Db db = Db::open(path);
    Library lib(db);  // migrates the copy
    ListQuery q;
    q.type = TypeFilter::WithFile;
    q.limit = 100000;
    const auto t0 = std::chrono::steady_clock::now();
    const auto rows = lib.rows(q);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int many = 0;
    std::string example;
    for (const auto& r : rows)
        if (r.files > 1) {
            ++many;
            if (example.empty()) example = r.key;
        }
    MESSAGE(rows.size() << " items with a file, " << many << " with more than one; all rows in " << ms << " ms");
    CHECK(many > 0);
    if (!example.empty())
        for (const auto& f : lib.fileChoices(example)) MESSAGE(example << ": " << f.title << " — " << FileChoiceScreen::fileMeta(f));
    q.downloaded = true;
    MESSAGE("downloaded: " << lib.countRows(q));
}
