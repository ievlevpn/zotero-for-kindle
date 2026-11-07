#include <doctest/doctest.h>

#include "docs/engine_factory.h"
#include "golden.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

using namespace zk;
using json = nlohmann::json;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"}; }

std::unique_ptr<DocumentEngine> openBook(const std::string& name, UserError* err = nullptr) {
    UserError e;
    auto engine = loadEngine(DocType::Epub, env(), &e);
    REQUIRE_MESSAGE(engine, e.details);
    if (!engine->open("tests/data/epub/" + name, err ? err : &e)) return nullptr;
    return engine;
}

int ink(const PageImage& p) {
    int n = 0;
    for (uint8_t g : p.gray) n += g < 100;
    return n;
}

std::vector<AnnotationRecord> zoteroHighlights(const std::string& book, size_t count) {
    std::ifstream in("tests/data/cfi/" + book + ".zotero.json");
    json records = json::parse(in);
    std::vector<AnnotationRecord> out;
    for (size_t i = 0; i < records.size() && out.size() < count; i += records.size() / count) {
        AnnotationRecord a;
        a.key = "ANN" + std::to_string(i);
        a.type = i % 2 ? "underline" : "highlight";
        a.color = "#ffd400";
        a.position = json{{"type", "FragmentSelector"}, {"value", records[i]["cfi"]}}.dump();
        out.push_back(a);
    }
    return out;
}

}  // namespace

TEST_CASE("epub engine: pages, positions and re-pagination") {
    auto e = openBook("pride-and-prejudice.epub");
    REQUIRE(e);
    CHECK(e->pageCount() > 100);
    CHECK(ink(e->render(10)) > 1000);
    const DocPosition pos = e->positionOfPage(57);
    CHECK_FALSE(pos.value.empty());
    CHECK(e->pageOf(pos) == 57);
    const int before = e->pageCount();
    Layout bigger;
    bigger.sizePt = 12;
    e->setLayout(bigger);
    CHECK(e->pageCount() > before);
    const int moved = e->pageOf(pos);
    CHECK(moved > 57);
    // The same passage is on that page: its position is between this page's start and the next.
    CHECK(e->pageOf(e->positionOfPage(moved)) == moved);
}

TEST_CASE("epub engine: contents, chapters, search and words") {
    auto e = openBook("pride-and-prejudice.epub");
    REQUIRE(e);
    auto toc = e->toc();
    REQUIRE(toc.size() > 10);
    int ch3 = -1;
    for (const auto& t : toc)
        if (t.title.find("III") != std::string::npos || t.title.find("3") != std::string::npos) {
            ch3 = t.page;
            break;
        }
    REQUIRE(ch3 > 0);
    CHECK(e->chapterAt(ch3 + 1) == e->chapterAt(ch3));
    CHECK_FALSE(e->chapterAt(ch3).empty());
    auto hits = e->search("Bingley", 10);
    REQUIRE(hits.size() == 10);
    for (const auto& h : hits) CHECK(h.snippet.find("Bingley") != std::string::npos);
    CHECK(hits[1].page >= hits[0].page);
    CHECK(e->wordsBetween(10, 12) > 200);
}

TEST_CASE("epub engine: Zotero highlights land on their pages") {
    auto e = openBook("pride-and-prejudice.epub");
    REQUIRE(e);
    auto anns = zoteroHighlights("pride-and-prejudice", 3);
    REQUIRE(anns.size() == 3);
    for (const auto& a : anns) {
        const int page = e->pageOfAnnotation(a);
        REQUIRE(page >= 0);
        auto boxes = e->highlights(page, {a});
        REQUIRE_FALSE(boxes.empty());
        for (const auto& b : boxes) {
            CHECK(b.annotationKey == a.key);
            CHECK(b.rect.x >= 0);
            CHECK(b.rect.y >= 0);
            CHECK(b.rect.x + b.rect.w <= 1236);
            CHECK(b.rect.y + b.rect.h <= 1487);
            CHECK(b.shade == 220);
            CHECK(b.underline == (a.type == "underline"));
        }
    }
    AnnotationRecord lost = anns[0];
    lost.position = R"j({"type":"FragmentSelector","value":"epubcfi(/6/1998!/4/2/1:0)"})j";
    CHECK(e->pageOfAnnotation(lost) == -1);
    CHECK(e->highlights(5, {lost}).empty());
}

TEST_CASE("epub engine: damaged, protected and TOC-less books") {
    UserError err;
    CHECK_FALSE(openBook("broken.epub", &err));
    CHECK(err.message == "This EPUB is damaged and can't be opened.");
    CHECK_FALSE(openBook("drm.epub", &err));
    CHECK(err.message == "This EPUB is protected (DRM) and can't be opened.");
    auto plain = openBook("no-toc.epub");
    REQUIRE(plain);
    CHECK_NOTHROW(plain->toc());  // crengine may build a fallback TOC from the spine
    CHECK(plain->pageCount() >= 1);
}

TEST_CASE("epub engine: Russian text hyphenates with the dictionary (golden)") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    auto e = openBook("problems-1001-ru.epub");
    REQUIRE(e);
    PageImage p = e->render(30);
    Canvas c(p.width, p.height);
    unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x) {
            const uint8_t g = p.gray[static_cast<size_t>(y) * p.width + x];
            reinterpret_cast<uint32_t*>(data + y * stride)[x] = 0xff000000u | (g << 16) | (g << 8) | g;
        }
    cairo_surface_mark_dirty(c.surface());
    test::checkGolden(c, "tests/golden/epub-ru-p30.png");
}

TEST_CASE("epub engine: a book with a non-Latin file name opens") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "zk-ru-name";
    fs::create_directories(dir);
    const fs::path copy = dir / "Соловьёв и Ларионов.epub";
    fs::copy_file("tests/data/epub/alice-gutenberg-epub3.epub", copy, fs::copy_options::overwrite_existing);
    UserError e;
    auto engine = loadEngine(DocType::Epub, env(), &e);
    REQUIRE(engine);
    CHECK_MESSAGE(engine->open(copy.string(), &e), e.message << " / " << e.details);
    CHECK(engine->pageCount() > 10);
    fs::remove_all(dir);
}

TEST_CASE("engines: a missing file says it's missing, not damaged") {
    UserError e;
    auto epub = loadEngine(DocType::Epub, env(), &e);
    REQUIRE(epub);
    CHECK_FALSE(epub->open("/tmp/zk-no-such-book.epub", &e));
    CHECK(e.message == "This book's file is missing. Open it from the library to download it again.");
    auto pdf = loadEngine(DocType::Pdf, EngineEnv{}, &e);
    REQUIRE(pdf);
    CHECK_FALSE(pdf->open("/tmp/zk-no-such-paper.pdf", &e));
    CHECK(e.message == "This book's file is missing. Open it from the library to download it again.");
}
