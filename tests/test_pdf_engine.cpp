#include <doctest/doctest.h>

#include "docs/engine_factory.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace zk;

namespace {

std::unique_ptr<DocumentEngine> openPdf(const std::string& name, UserError* err = nullptr) {
    UserError e;
    auto engine = loadEngine(DocType::Pdf, EngineEnv{}, &e);
    REQUIRE_MESSAGE(engine, e.details);
    if (!engine->open("tests/data/pdf/" + name, err ? err : &e)) return nullptr;
    return engine;
}

}  // namespace

TEST_CASE("pdf engine: pages, outline, search") {
    auto e = openPdf("sample.pdf");
    REQUIRE(e);
    CHECK(e->pageCount() == 3);
    PageImage p = e->render(0);
    CHECK(p.width == 1236);
    CHECK(p.height == 1487);
    int ink = 0;
    for (uint8_t g : p.gray) ink += g < 100;
    CHECK(ink > 1000);
    CHECK(e->render(1).gray != p.gray);
    auto toc = e->toc();
    REQUIRE(toc.size() == 3);
    CHECK(toc[1].title == "Chapter two");
    CHECK(toc[1].page == 1);
    CHECK(e->chapterAt(2) == "Chapter three");
    auto hits = e->search("Hamiltonian", 10);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].page == 1);
    CHECK(hits[0].snippet.find("Hamiltonian") != std::string::npos);
    CHECK(e->pageOf(e->positionOfPage(2)) == 2);
    CHECK(e->wordsBetween(0, 2) > 50);
}

TEST_CASE("pdf engine: a Zotero PDF highlight maps from PDF space to the page") {
    auto e = openPdf("sample.pdf");
    REQUIRE(e);
    AnnotationRecord a;
    a.key = "PDFANN01";
    a.type = "highlight";
    a.color = "#2ea8e5";
    a.position = R"({"pageIndex":1,"rects":[[72,700,300,712]]})";
    CHECK(e->pageOfAnnotation(a) == 1);
    auto boxes = e->highlights(1, {a});
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].rect.y < 200);  // PDF y grows upwards: 700 is near the top
    CHECK(boxes[0].rect.x > 50);
    CHECK(boxes[0].rect.w > 200);
    CHECK(boxes[0].shade == 180);
    CHECK(e->highlights(0, {a}).empty());  // another page
    a.position = "garbage";
    CHECK(e->pageOfAnnotation(a) == -1);
}

TEST_CASE("pdf engine: password-protected and damaged files") {
    UserError err;
    CHECK_FALSE(openPdf("encrypted.pdf", &err));
    CHECK(err.message == "This PDF is password-protected and can't be opened.");
    CHECK_FALSE(openPdf("../epub/broken.epub", &err));
    CHECK(err.message == "This PDF is damaged and can't be opened.");
}

TEST_CASE("pdf engine: the module exports only its factory") {
    const char* env = std::getenv("ZK_LIB_DIR");
    const std::string lib = std::string(env ? env : "build-desktop") + "/libzk-pdf.so";
#ifdef __APPLE__
    REQUIRE(std::system(("nm -gU " + lib + " > /tmp/zk-nm-pdf.txt").c_str()) == 0);
#else
    REQUIRE(std::system(("nm -D --defined-only " + lib + " > /tmp/zk-nm-pdf.txt").c_str()) == 0);
#endif
    std::ifstream in("/tmp/zk-nm-pdf.txt");
    std::stringstream ss;
    ss << in.rdbuf();
    int defined = 0;
    std::istringstream ls(ss.str());
    for (std::string line; std::getline(ls, line);)
        if (line.find(" T ") != std::string::npos || line.find(" D ") != std::string::npos ||
            line.find(" S ") != std::string::npos || line.find(" B ") != std::string::npos)
            ++defined;
    CHECK(defined == 1);
    CHECK(ss.str().find("zk_pdf_engine_create") != std::string::npos);
}
