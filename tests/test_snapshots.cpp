// Web snapshots (0.7 spec B): Zotero's saved web pages download (a zip of the page and its files),
// unpack into files/<key>/ and open in the EPUB engine; "Web" in lists; no highlights, only Search and
// Search on a selection; the place kept on the Kindle only.
//
// The real page is a SingleFile snapshot from Pavel's library (Verbitsky's "Антикопирайт", its
// picture inlined); page/ is an older-style snapshot: the page, its picture and style sheet beside it.

#include <doctest/doctest.h>

#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_http.h"
#include "fake_nav.h"
#include "fake_zotero.h"
#include "golden.h"
#include "library_nav_fit.h"
#include "net/write_guard.h"
#include "screens/annotation_bar.h"
#include "screens/book_search.h"
#include "screens/file_choice.h"
#include "screens/library.h"
#include "screens/open_flow.h"
#include "screens/reader.h"
#include "store/db.h"
#include "store/library.h"
#include "sync/snapshot_zip.h"
#include "sync/sync_engine.h"
#include "sync/webdav.h"
#include "sync/zotero_storage.h"
#include "ui/item_icons.h"
#include "ui/metrics.h"
#include "ui/text.h"
#include "zotero/api.h"

#include <miniz.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace zk;
using namespace zk::test;
namespace fs = std::filesystem;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}; }

const std::string kRealName = "\xD0\x92\xD0\xB5\xD1\x80\xD0\xB1\xD0\xB8\xD1\x86\xD0\xBA\xD0\xB8\xD0\xB9 - "
                              "\xD0\x90\xD0\xBD\xD1\x82\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82.html";
const std::string kReal = "tests/data/snapshot/" + kRealName;
const std::string kRealMd5 = "f3ad538e33a763ce45dcaaf085506345";  // as Zotero has it for YNIQCXPZ
const std::string kPage = "tests/data/snapshot/page/page.html";
const std::string kPageMd5 = "2dd8b8b6b2bb51c52eb31b04d1e4c771";
const std::string kDav = "https://dav.example.ts.net:8443/zotero/";

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::unique_ptr<DocumentEngine> openPage(const std::string& path, UserError* err = nullptr) {
    UserError e;
    auto engine = loadEngine(DocType::Epub, env(), &e);
    REQUIRE_MESSAGE(engine, e.details);
    if (!engine->open(path, err ? err : &e)) return nullptr;
    return engine;
}

int ink(const PageImage& p) {
    int n = 0;
    for (uint8_t g : p.gray) n += g < 100;
    return n;
}

bool fonts() {
    if (!fs::exists("assets/fonts-device/Bookerly-Regular.ttf")) return false;
    addFontDir("assets/fonts-device");
    return true;
}

struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/zk-snap-XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { fs::remove_all(path); }
};

AttachmentRecord realSnapshot() {
    AttachmentRecord a;
    a.key = "YNIQCXPZ";
    a.parentKey = "S6DJB2YV";
    a.contentType = "text/html";
    a.filename = kRealName;
    a.md5 = kRealMd5;
    a.linkMode = "imported_url";
    a.mtime = 1701287461002;
    a.title = "Антикопирайт";
    return a;
}

AttachmentRecord multiSnapshot() {
    AttachmentRecord a;
    a.key = "MULTI001";
    a.parentKey = "WEBITEM1";
    a.contentType = "text/html";
    a.filename = "page.html";
    a.md5 = kPageMd5;
    a.linkMode = "imported_url";
    a.mtime = 1700000000000;
    return a;
}

// A zip written here: (name, bytes) entries; `symlink` marks that entry as a Unix symlink.
std::string makeZip(const std::vector<std::pair<std::string, std::string>>& entries, const std::string& symlink = "") {
    mz_zip_archive zip{};
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    // miniz won't write a name starting with "/" or holding "\\": such a name goes in as a
    // stand-in of its length ("@@@…"), swapped back in the bytes after.
    std::vector<std::pair<std::string, std::string>> swaps;
    for (const auto& [name, bytes] : entries) {
        std::string stored = name;
        if (name[0] == '/' || name.find('\\') != std::string::npos) {
            stored = std::string(name.size(), '@');
            swaps.push_back({stored, name});
        }
        REQUIRE(mz_zip_writer_add_mem(&zip, stored.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION));
    }
    void* buf = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
    std::string out(static_cast<const char*>(buf), size);
    mz_zip_writer_end(&zip);
    for (const auto& [stored, name] : swaps)
        for (size_t at = out.find(stored); at != std::string::npos; at = out.find(stored, at + 1)) out.replace(at, stored.size(), name);
    if (!symlink.empty()) {
        // Central directory entries: signature PK\1\2, external attributes at offset 38.
        for (size_t at = out.find("PK\x01\x02"); at != std::string::npos; at = out.find("PK\x01\x02", at + 4)) {
            const uint16_t nameLen = static_cast<uint8_t>(out[at + 28]) | static_cast<uint8_t>(out[at + 29]) << 8;
            if (out.compare(at + 46, nameLen, symlink) != 0) continue;
            const uint32_t attr = static_cast<uint32_t>(S_IFLNK | 0777) << 16;
            for (int i = 0; i < 4; ++i) out[at + 38 + i] = static_cast<char>((attr >> (8 * i)) & 0xFF);
            out[at + 5] = 3;  // made by Unix (attributes in the high 16 bits)
        }
    }
    return out;
}

// Every regular file under `dir`, relative.
std::vector<std::string> filesUnder(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file()) out.push_back(fs::relative(it->path(), dir).string());
    std::sort(out.begin(), out.end());
    return out;
}

// A library with web snapshots: WEBITEM1, a web page saved as a snapshot only; BOTHITEM, an
// article with a PDF and a snapshot; BOOKITEM, a book whose only file is a saved page (shown as
// Web); LINKITEM, a web page with only a link (no file: nothing to read).
struct LibRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    LibRig() {
        auto item = [&](const std::string& key, const std::string& type, const std::string& title, const std::string& added) {
            ItemRecord r;
            r.key = key;
            r.version = 1;
            r.itemType = type;
            r.title = title;
            r.creators = {{"Dmitry", "Kaledin", "", "author"}};
            r.dateAdded = r.dateModified = added;
            r.json = "{}";
            lib.upsertItem(r);
        };
        item("WEBITEM1", "webpage", "A page with its files", "2025-03-04T00:00:00Z");
        item("BOTHITEM", "journalArticle", "Pricing with snapshots", "2025-03-03T00:00:00Z");
        item("BOOKITEM", "book", "\xD0\x90\xD0\xBD\xD1\x82\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82",
             "2025-03-02T00:00:00Z");
        item("LINKITEM", "webpage", "Only a link", "2025-03-01T00:00:00Z");
        lib.upsertAttachment(multiSnapshot());
        lib.upsertAttachment({"PDFBOTH1", "BOTHITEM", "application/pdf", "paper.pdf", "aa", "imported_url", 0, "",
                              AttachmentRecord::State::Remote, "Full Text PDF"});
        lib.upsertAttachment({"SNAPBOTH", "BOTHITEM", "text/html", "paper.html", "bb", "imported_url", 0, "",
                              AttachmentRecord::State::Remote, "Snapshot"});
        AttachmentRecord real = realSnapshot();
        real.parentKey = "BOOKITEM";
        lib.upsertAttachment(real);
        lib.upsertAttachment({"LINK0001", "LINKITEM", "text/html", "", "", "linked_url", 0, "", AttachmentRecord::State::Remote,
                              "Link"});
    }
};

}  // namespace

// ---- The engine ----------------------------------------------------------------------------

TEST_CASE("snapshots: the EPUB engine opens a saved web page, with contents from its headings and search") {
    UserError err;
    auto e = openPage(kReal, &err);
    REQUIRE_MESSAGE(e, err.details);
    CHECK(e->pageCount() >= 2);
    CHECK(ink(e->render(0)) > 1000);
    auto toc = e->toc();
    REQUIRE_FALSE(toc.empty());
    CHECK(toc[0].title.find("\xD0\x90\xD0\x9D\xD0\xA2\xD0\x98\xD0\x9A\xD0\x9E\xD0\x9F\xD0\x98\xD0\xA0\xD0\x90\xD0\x99\xD0\xA2") !=
          std::string::npos);  // АНТИКОПИРАЙТ
    auto hits = e->search("\xD0\xBA\xD0\xBE\xD0\xBF\xD0\xB8\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82", 5);  // копирайт
    CHECK_FALSE(hits.empty());
    // A web page has no CFIs: no Zotero position, no highlights.
    CHECK(e->zoteroPosition(1).empty());
    AnnotationRecord a;
    a.key = "SNAPANN1";
    a.position = R"j({"type":"CssSelector","value":"p:nth-child(2)"})j";
    CHECK(e->highlights(0, {a}).empty());
    CHECK(e->pageOfAnnotation(a) == -1);
    // Positions round-trip, so the place is kept.
    const DocPosition pos = e->positionOfPage(1);
    CHECK_FALSE(pos.value.empty());
    CHECK(e->pageOf(pos) == 1);
}

TEST_CASE("snapshots: a selection on a web page is its words, with nothing to place an annotation with") {
    auto e = openPage(kReal);
    REQUIRE(e);
    std::optional<Selection> sel;
    for (int y = 200; y < 1300 && !(sel && !sel->text.empty()); y += 40) sel = e->select(1, {300, y}, {800, y});
    REQUIRE(sel);
    CHECK_FALSE(sel->text.empty());
    CHECK_FALSE(sel->rects.empty());
    CHECK(sel->position.empty());
    CHECK_FALSE(sel->roundTrips);
}

TEST_CASE("snapshots: a page's pictures and style sheet come from its own folder; scripts don't run") {
    auto with = openPage(kPage);
    REQUIRE(with);
    const PageImage p = with->render(0);
    auto toc = with->toc();
    REQUIRE(toc.size() == 2);
    CHECK(toc[0].title == "A page with its files");
    CHECK(toc[1].title == "Second part");
    CHECK(with->search("scripts never run", 2).empty());  // the script's text is not shown
    // The same page without its folder: the checkerboard is missing.
    TempDir t;
    fs::copy_file(kPage, t.path / "page.html");
    auto without = openPage((t.path / "page.html").string());
    REQUIRE(without);
    CHECK(ink(p) > ink(without->render(0)) + 20000);
}

TEST_CASE("snapshots: a file that is neither an EPUB nor a web page is damaged") {
    TempDir t;
    std::ofstream(t.path / "x.html", std::ios::binary) << "%PDF-1.4 not a page";
    UserError err;
    CHECK_FALSE(openPage((t.path / "x.html").string(), &err));
    CHECK(err.message == "This EPUB is damaged and can't be opened.");
}

// ---- Downloads -----------------------------------------------------------------------------

TEST_CASE("snapshots: a WebDAV snapshot zip downloads, unpacks into files/<key>/ and opens") {
    TempDir dest;
    FakeHttp http;
    WebDav dav(http, kDav, "u", "p");
    dav.setFreeSpaceProbe([](const std::string&) { return uint64_t{1} << 40; });
    http.expect("GET", kDav + "YNIQCXPZ.prop",
                FakeHttp::ok("<properties version=\"1\"><mtime>1701287461002</mtime><hash>" + kRealMd5 + "</hash></properties>"));
    http.expect("GET", kDav + "YNIQCXPZ.zip", FakeHttp::ok(slurp("tests/data/snapshot/YNIQCXPZ.zip")));
    UserError err;
    REQUIRE_MESSAGE(dav.download(realSnapshot(), dest.path.string(), &err), err.details);
    const fs::path page = dest.path / "YNIQCXPZ" / kRealName;
    CHECK(slurp(page.string()) == slurp(kReal));
    struct stat st {};
    REQUIRE(::stat(page.c_str(), &st) == 0);
    CHECK(st.st_mtime == 1701287461);
    CHECK(filesUnder(dest.path) == std::vector<std::string>{"YNIQCXPZ/" + kRealName});  // nothing else left
    CHECK(fs::path(localFileName(realSnapshot())).filename() == kRealName);
    auto e = openPage(page.string());
    REQUIRE(e);
    CHECK(e->pageCount() >= 2);
}

TEST_CASE("snapshots: a page with its files unpacks whole, replacing an older copy, and shows its picture") {
    TempDir dest;
    fs::create_directories(dest.path / "MULTI001" / "old_files");
    std::ofstream(dest.path / "MULTI001" / "old_files" / "stale.png") << "x";
    FakeHttp http;
    WebDav dav(http, kDav, "u", "p");
    dav.setFreeSpaceProbe([](const std::string&) { return uint64_t{1} << 40; });
    http.expect("GET", kDav + "MULTI001.prop", FakeHttp::ok("<properties version=\"1\"><mtime>0</mtime><hash>" + kPageMd5 + "</hash></properties>"));
    http.expect("GET", kDav + "MULTI001.zip", FakeHttp::ok(slurp("tests/data/snapshot/MULTI001.zip")));
    UserError err;
    REQUIRE_MESSAGE(dav.download(multiSnapshot(), dest.path.string(), &err), err.details);
    CHECK(filesUnder(dest.path) ==
          std::vector<std::string>{"MULTI001/page.html", "MULTI001/page_files/figure.png", "MULTI001/page_files/style.css"});
    auto e = openPage((dest.path / "MULTI001" / "page.html").string());
    REQUIRE(e);
    auto reference = openPage(kPage);
    REQUIRE(reference);
    CHECK(ink(e->render(0)) == ink(reference->render(0)));
}

TEST_CASE("snapshots: Zotero storage sends a lone page as it is, or a zip (marked or not) that is unpacked") {
    const std::string fileUrl = "https://api.zotero.org/users/42/items/YNIQCXPZ/file";
    const std::string s3 = "https://zoterofilestorage.s3.amazonaws.com/x?sig=1";
    for (int mode : {0, 1, 2}) {  // 0 the page, 1 a zip marked compressed, 2 a zip unmarked
        CAPTURE(mode);
        TempDir dest;
        fs::create_directories(dest.path / "YNIQCXPZ");
        std::ofstream(dest.path / "YNIQCXPZ" / "old.png") << "an older copy's file";
        FakeHttp http;
        ZoteroStorage store(http, 42, "abcdefghijklmnopqrstuvwx");
        store.setFreeSpaceProbe([](const std::string&) { return uint64_t{1} << 40; });
        std::map<std::string, std::string> headers{{"location", s3}};
        if (mode == 1) headers["zotero-file-compressed"] = "Yes";
        http.expect("GET", fileUrl, FakeHttp::status(302, headers));
        http.expect("GET", s3, FakeHttp::ok(mode == 0 ? slurp(kReal) : slurp("tests/data/snapshot/YNIQCXPZ.zip")));
        UserError err;
        REQUIRE_MESSAGE(store.download(realSnapshot(), dest.path.string(), &err), err.details);
        CHECK(filesUnder(dest.path) == std::vector<std::string>{"YNIQCXPZ/" + kRealName});
        CHECK(slurp((dest.path / "YNIQCXPZ" / kRealName).string()) == slurp(kReal));
    }
}

TEST_CASE("snapshots: a zip whose page doesn't match Zotero's checksum is refused, leaving nothing") {
    TempDir dest;
    FakeHttp http;
    WebDav dav(http, kDav, "u", "p");
    dav.setFreeSpaceProbe([](const std::string&) { return uint64_t{1} << 40; });
    http.expect("GET", kDav + "YNIQCXPZ.prop", FakeHttp::ok("<properties><mtime>1</mtime><hash>x</hash></properties>"));
    http.expect("GET", kDav + "YNIQCXPZ.zip", FakeHttp::ok(slurp("tests/data/snapshot/YNIQCXPZ.zip")));
    AttachmentRecord a = realSnapshot();
    a.md5 = "00000000000000000000000000000000";
    UserError err;
    CHECK_FALSE(dav.download(a, dest.path.string(), &err));
    CHECK(err.kind == ErrorKind::Checksum);
    CHECK(fs::is_empty(dest.path));
}

TEST_CASE("snapshots: unsafe zips are refused before anything is written") {
    const std::string page = slurp(kPage);
    struct Case {
        const char* what;
        std::string zip;
        SnapshotLimits limits;
        const char* why;  // the details start so
    };
    std::vector<Case> cases = {
        {"a path out of the folder", makeZip({{"page.html", page}, {"../escape.html", "x"}}), {}, "unsafe entry"},
        {"a deeper way out", makeZip({{"page.html", page}, {"page_files/../../escape.html", "x"}}), {}, "unsafe entry"},
        {"an absolute path", makeZip({{"page.html", page}, {"/tmp/zk-snap-escape.html", "x"}}), {}, "unsafe entry"},
        {"a backslash", makeZip({{"page.html", page}, {"..\\escape.html", "x"}}), {}, "unsafe entry"},
        {"a dot part", makeZip({{"page.html", page}, {"./page_files/a.png", "x"}}), {}, "unsafe entry"},
        {"a symlink", makeZip({{"page.html", page}, {"page_files/link", "/etc/passwd"}}, "page_files/link"), {}, "symlink"},
        {"too many entries", makeZip({{"page.html", page}, {"a.png", "1"}, {"b.png", "2"}}), {1u << 20, 2}, "3 entries"},
        {"too big unpacked", makeZip({{"page.html", page}, {"big.bin", std::string(4096, 'z')}}), {2048, 100}, "over"},
        {"not a zip", "this is not a zip", {}, "zip:"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.what);
        TempDir dest;
        const fs::path zip = dest.path / ".MULTI001.zip.part";
        std::ofstream(zip, std::ios::binary) << c.zip;
        UserError err;
        CHECK_FALSE(installSnapshot(multiSnapshot(), zip.string(), dest.path.string(), 0, [] { return uint64_t{1} << 40; },
                                    &err, c.limits));
        CHECK(err.kind == ErrorKind::Other);
        CHECK(err.message == "The server's copy of page.html is damaged.");
        CHECK(err.details.rfind(c.why, 0) == 0);
        fs::remove(zip);
        CHECK(fs::is_empty(dest.path));
        CHECK_FALSE(fs::exists(dest.path.parent_path() / "escape.html"));
        CHECK_FALSE(fs::exists("/tmp/zk-snap-escape.html"));
    }
    // A safe one passes the same way.
    TempDir dest;
    const fs::path zip = dest.path / ".MULTI001.zip.part";
    std::ofstream(zip, std::ios::binary) << makeZip({{"page.html", page}, {"page_files/", ""}, {"page_files/a.png", "1"}});
    UserError err;
    CHECK(installSnapshot(multiSnapshot(), zip.string(), dest.path.string(), 0, [] { return uint64_t{1} << 40; }, &err));
}

TEST_CASE("snapshots: a zip larger than the free space is DiskFull") {
    TempDir dest;
    const fs::path zip = dest.path / ".MULTI001.zip.part";
    std::ofstream(zip, std::ios::binary) << slurp("tests/data/snapshot/MULTI001.zip");
    UserError err;
    CHECK_FALSE(installSnapshot(multiSnapshot(), zip.string(), dest.path.string(), 0, [] { return uint64_t{1000}; }, &err));
    CHECK(err.kind == ErrorKind::DiskFull);
    fs::remove(zip);
    CHECK(fs::is_empty(dest.path));
}

// ---- The library ---------------------------------------------------------------------------

TEST_CASE("snapshots: a saved page is a Web file: its badge, the Web filter, and With a file") {
    LibRig rig;
    auto web = rig.lib.rowFor("WEBITEM1");
    REQUIRE(web);
    CHECK(web->badge == "WEB");
    CHECK(web->attachmentKey == "MULTI001");
    CHECK(web->files == 1);
    auto book = rig.lib.rowFor("BOOKITEM");
    REQUIRE(book);
    CHECK(book->badge == "WEB");
    // With a PDF too: the PDF is what the row opens; two files.
    auto both = rig.lib.rowFor("BOTHITEM");
    REQUIRE(both);
    CHECK(both->badge == "PDF");
    CHECK(both->files == 2);
    // A link alone is no file.
    auto link = rig.lib.rowFor("LINKITEM");
    REQUIRE(link);
    CHECK(link->badge.empty());
    CHECK(link->files == 0);

    auto keys = [&](TypeFilter t) {
        ListQuery q;
        q.type = t;
        q.limit = 50;
        std::vector<std::string> out;
        for (const auto& r : rig.lib.rows(q)) out.push_back(r.key);
        CHECK(rig.lib.countRows(q) == static_cast<int64_t>(out.size()));
        return out;
    };
    CHECK(keys(TypeFilter::Web) == std::vector<std::string>{"WEBITEM1", "BOOKITEM"});
    CHECK(keys(TypeFilter::WithFile) == std::vector<std::string>{"WEBITEM1", "BOTHITEM", "BOOKITEM"});
    CHECK(keys(TypeFilter::Pdf) == std::vector<std::string>{"BOTHITEM"});
    CHECK(keys(TypeFilter::Any).size() == 4);
    // "On Kindle" counts a downloaded page.
    ListQuery q;
    q.downloaded = true;
    CHECK(rig.lib.countRows(q) == 0);
    rig.lib.setFileState("MULTI001", AttachmentRecord::State::Local, "/tmp/MULTI001/page.html");
    CHECK(rig.lib.countRows(q) == 1);

    LibraryView v;
    v.type = TypeFilter::Web;
    CHECK(filterLine(v, 2) == "Web · 2 items");
    saveLibraryView(rig.lib, v);
    CHECK(loadLibraryView(rig.lib).type == TypeFilter::Web);
}

TEST_CASE("snapshots: a Web row shows the globe with WEB under it, whatever the item's type") {
    auto draw = [](const std::string& type, const std::string& format) {
        Canvas c(100, 110);
        cairo_set_source_rgb(c.cr(), 1, 1, 1);
        cairo_paint(c.cr());
        itemTypeIcon(c.cr(), {10, 10, 74, 92}, type, format);
        return grayOf(c);
    };
    CHECK(draw("book", "WEB") == draw("webpage", "WEB"));
    CHECK(draw("book", "WEB") != draw("book", "PDF"));
    CHECK(draw("webpage", "WEB") != draw("webpage", ""));  // the label is drawn
}

TEST_CASE("snapshots: an item with a PDF and a snapshot asks which file, PDF first") {
    LibRig rig;
    FakeNav nav{rig.lib};
    OpenFlow flow{[] { return int64_t{1'762'500'000'000}; }};
    auto files = rig.lib.fileChoices("BOTHITEM");
    REQUIRE(files.size() == 2);
    CHECK(files[0].attachment.key == "PDFBOTH1");
    CHECK(files[1].attachment.key == "SNAPBOTH");
    CHECK(FileChoiceScreen::fileMeta(files[1]) == "Web page");
    flow.open(*rig.lib.rowFor("BOTHITEM"), nav);
    REQUIRE(nav.pushed.size() == 1);
    auto* sheet = dynamic_cast<FileChoiceScreen*>(nav.pushed[0].get());
    REQUIRE(sheet);
    sheet->onGesture(tapAt(sheet->fileRects()[1].center()), nav);
    REQUIRE(nav.syncFake.opened.size() == 1);
    CHECK(nav.syncFake.opened[0] == "BOTHITEM/SNAPBOTH");
    // The exact row for that file says WEB.
    auto row = rig.lib.rowFor("BOTHITEM", "SNAPBOTH");
    REQUIRE(row);
    CHECK(row->badge == "WEB");
    CHECK(row->exactFile);
    // Its details list the PDF, then the page.
    auto d = rig.lib.details("BOTHITEM");
    REQUIRE(d);
    REQUIRE(d->files.size() == 2);
    CHECK(d->files[1].attachment.key == "SNAPBOTH");
    CHECK(rig.lib.filesOfManyFileItems() == std::vector<std::string>{"PDFBOTH1", "SNAPBOTH"});
}

TEST_CASE("snapshots: an older library learns its snapshots once; Zotero's snapshot annotations go") {
    const fs::path path = fs::temp_directory_path() / "zk-snapshots-upgrade.db";
    fs::remove(path);
    {
        Db db = Db::open(path.string());
        Library lib(db);
        ItemRecord r;
        r.key = "WEBITEM1";
        r.itemType = "webpage";
        r.title = "Saved before 0.7";
        lib.upsertItem(r);
        lib.upsertAttachment(multiSnapshot());
        AnnotationRecord a;
        a.key = "SNAPANN1";
        a.parentKey = "MULTI001";
        a.type = "highlight";
        a.position = R"j({"type":"CssSelector","value":"p"})j";
        lib.upsertAnnotation(a);
        // As 0.6 left it: no readable file, the annotation kept.
        db.exec("UPDATE items SET readableKey = '', readableType = ''; DELETE FROM meta WHERE key = 'webSnapshots'");
    }
    Db db = Db::open(path.string());
    Library lib(db);
    auto row = lib.rowFor("WEBITEM1");
    REQUIRE(row);
    CHECK(row->badge == "WEB");
    CHECK(row->attachmentKey == "MULTI001");
    CHECK_FALSE(lib.annotation("SNAPANN1"));
    CHECK(lib.metaValue("webSnapshots") == "1");
    fs::remove(path);
}

TEST_CASE("snapshots: the sync doesn't keep Zotero's annotations on a snapshot") {
    FakeZotero server;
    Db db = Db::open(":memory:");
    Library lib(db);
    lib.setAllAnnotationsOffline(true);
    WriteGuard guard(server, [&](const std::string& k) { return lib.annotation(k).has_value(); },
                     [&](const std::string& k) { return lib.attachment(k).has_value(); });
    ZoteroApi api(guard, 1, "k3y");
    api.setSleeper([](int) {});
    SyncEngine engine(api, lib);
    server.put(FakeZotero::item("WEBITEM1", "A page"));
    auto snap = FakeZotero::attachment("MULTI001", "WEBITEM1", kPageMd5);
    snap["contentType"] = "text/html";
    snap["linkMode"] = "imported_url";
    snap["filename"] = "page.html";
    server.put(snap);
    server.put(FakeZotero::attachment("EPUB0001", "WEBITEM1", "md5"));
    auto onPage = FakeZotero::annotation("SNAPANN1", "MULTI001", "on the page");
    onPage["annotationPosition"] = R"j({"type":"CssSelector","value":"p"})j";
    server.put(onPage);
    server.put(FakeZotero::annotation("EPUBANN1", "EPUB0001", "in the book"));
    REQUIRE_FALSE(engine.pull([](const SyncProgress&) {}).error);
    CHECK_FALSE(lib.annotation("SNAPANN1"));
    CHECK(lib.annotation("EPUBANN1"));
    CHECK(lib.rowFor("WEBITEM1")->badge == "EPUB");
    CHECK(lib.rowFor("WEBITEM1")->files == 2);
}

// ---- The reader ----------------------------------------------------------------------------

namespace {

struct ReaderRig {
    LibRig lib;
    FakeNav nav{lib.lib};
    std::unique_ptr<ReaderScreen> reader;
    ReaderRig() {
        addFontDir("assets/fonts-device");
        lib.lib.setFileState("YNIQCXPZ", AttachmentRecord::State::Local, kReal);
        open();
    }
    void open() {
        auto row = lib.lib.rowFor("BOOKITEM", "YNIQCXPZ");
        REQUIRE(row);
        reader = std::make_unique<ReaderScreen>(*row, kReal, DocType::Epub, [] {
            return std::make_unique<RenderWorker>([](DocType t) {
                UserError e;
                return loadEngine(t, env(), &e);
            });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
        REQUIRE(settle([&] { return reader->ready(); }));
    }
    bool settle(const std::function<bool()>& done, int ms = 8000) {
        for (int i = 0; i < ms / 5; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    // A long press on the first words found going down the page, lifted: the bar.
    bool select() {
        for (int y = 200; y < 1300; y += 60) {
            const Point p{400, y};
            reader->onGesture(Gesture{GestureType::LongPress, p, p}, nav);
            reader->onGesture(Gesture{GestureType::LongPressUp, p, p}, nav);
            if (settle([&] { return reader->annotationBar() != nullptr; }, 400)) return true;
        }
        return false;
    }
};

}  // namespace

TEST_CASE("snapshots: the reader opens a web page; the selection bar has no colours, only Search") {
    ReaderRig rig;
    CHECK(rig.reader->webPage());
    CHECK(rig.reader->pageCount() >= 2);
    REQUIRE(rig.select());
    AnnotationBar* bar = rig.reader->annotationBar();
    CHECK(bar->mode() == AnnotationBar::Mode::Text);
    CHECK_FALSE(bar->hasColours());
    CHECK(bar->rect().h == AnnotationBar::kTextH);
    CHECK(bar->hit(bar->buttonRect(0).center()).action == AnnotationBar::Action::Search);
    CHECK(bar->buttonRect(0).w == bar->rect().w - 40);  // the one button, the bar's width
    CHECK(bar->hit(bar->buttonRect(1).center()).action == AnnotationBar::Action::None);

    // Search: this page's search, for those words, at once; nothing saved.
    const std::string again = rig.reader->selection()->text;
    CHECK_FALSE(again.empty());
    rig.reader->onGesture(tapAt(bar->buttonRect(0).center()), rig.nav);
    CHECK_FALSE(rig.reader->annotationBar());
    REQUIRE(rig.nav.pushed.size() == 1);
    auto* search = dynamic_cast<BookSearchScreen*>(rig.nav.pushed[0].get());
    REQUIRE(search);
    search->onShow(rig.nav);
    CHECK(search->query() == again);
    CHECK(search->searching());
    CHECK_FALSE(rig.nav.keyboardFake.shown);
    for (int i = 0; i < 400 && search->searching(); ++i) {
        search->onTick(rig.nav);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(search->hits().empty());
    CHECK(rig.lib.lib.annotationsFor("YNIQCXPZ").empty());
}

TEST_CASE("snapshots: the Text bar offers a plugin's selection actions under More…") {
    struct Entries : PluginEntries {
        std::vector<PluginAction> actions(const std::string& kind) override {
            if (kind != "selectionAction") return {};
            return {{"explain", "Explainer", "explain", "Explain"}};
        }
        std::vector<PluginLaunch> mainScreens() override { return {}; }
        bool anyBadge() override { return false; }
        uint64_t version() override { return 1; }
        uint64_t run(const PluginAction&, const std::string&, nlohmann::ordered_json, nlohmann::json) override { return 1; }
        std::optional<ActionOutcome> take(uint64_t) override { return std::nullopt; }
        void stop(uint64_t) override {}
        void openScreen(const std::string&, const std::string&) override {}
    } entries;
    ReaderRig rig;
    rig.nav.entries = &entries;
    REQUIRE(rig.select());
    AnnotationBar* bar = rig.reader->annotationBar();
    CHECK(bar->hit(bar->buttonRect(0).center()).action == AnnotationBar::Action::Search);
    CHECK(bar->hit(bar->buttonRect(1).center()).action == AnnotationBar::Action::More);
    rig.reader->onGesture(tapAt(bar->buttonRect(1).center()), rig.nav);
    REQUIRE(bar->listing());
    CHECK(bar->listed()[0].title == "Explain");
    rig.nav.entries = nullptr;
}

TEST_CASE("snapshots: the reading position is kept on the Kindle, never sent, and Zotero's isn't used") {
    ReaderRig rig;
    // Zotero's snapshot position: a scroll percentage, not a page here.
    rig.lib.lib.applyRemotePosition("YNIQCXPZ", "0.42", 7);
    rig.open();
    CHECK(rig.reader->page() == 0);
    CHECK_FALSE(rig.reader->positionQuestion());
    rig.reader->turnTo(2);
    REQUIRE(rig.settle([&] { return rig.reader->page() == 2 && !rig.reader->turning(); }));
    const std::string place = rig.lib.lib.readingLocation("YNIQCXPZ");
    CHECK_FALSE(place.empty());
    CHECK(rig.lib.lib.dirtyPositions().empty());
    auto rp = rig.lib.lib.readingPosition("YNIQCXPZ");
    REQUIRE(rp);
    CHECK(rp->local.empty());
    // Opened again: the same page.
    rig.open();
    CHECK(rig.reader->page() == 2);
}

TEST_CASE("snapshots: a web page in the reader, and Web rows in the Library, match the reviewed goldens") {
    if (!fonts()) return;
    {
        ReaderRig rig;
        Canvas c(1236, 1547);
        rig.reader->draw(c);
        checkGolden(c, "tests/golden/snapshot-reader.png");
    }
    LibRig rig;
    FakeNav nav{rig.lib};
    LibraryScreen screen(nullptr);
    nav.visible = &screen;
    screen.onShow(nav);
    LibraryView v;
    v.type = TypeFilter::Any;
    screen.setView(v, nav);
    Canvas c(1236, 1547);
    screen.draw(c);
    checkGolden(c, "tests/golden/snapshot-rows.png");
}

TEST_CASE("snapshots: the Text bar and Web rows fit on the KT4 and the Scribe") {
    for (const Metrics* m : {&kKt4, &kScribe}) {
        ScopedMetrics s(*m);
        CAPTURE(m->device);
        const Rect page = ReaderScreen::pageArea();
        for (Rect anchor : {Rect{100, 100, 400, 40}, Rect{100, page.h / 2, 400, 40}, Rect{100, page.h - 80, 400, 40}})
            for (bool more : {false, true}) {
                AnnotationBar bar(AnnotationBar::Mode::Text, anchor, "#ffd400", more);
                CHECK(within(bar.rect(), page));
                CHECK(bar.rect().intersected(anchor).empty());
                for (int i = 0; i < 1 + more; ++i) {
                    CHECK(within(bar.buttonRect(i), bar.rect()));
                    if (i) CHECK(apart(bar.buttonRect(i - 1), bar.buttonRect(i)));
                }
                const int labelW = measureText("Search", {theme::kSans, 8, 500}, 1000, 1).width;
                CHECK(bar.buttonRect(0).w > labelW + 20);
            }
        // The Filter sheet's five Type chips (Web among them) fit.
        LibraryView v;
        FilterSheet filter(v);
        const Rect area = LibraryScreen::sheetArea();
        CHECK(filter.typeRects(area).size() == 5);
        for (const auto& r : filter.typeRects(area)) CHECK(within(r, filter.sheetRect(area)));
        // WEB fits under the icon as EPUB does.
        const TextStyle label{theme::kSans, 5, 700};
        CHECK(measureText("WEB", label, 200, 1).width <= measureText("EPUB", label, 200, 1).width);
    }
}
