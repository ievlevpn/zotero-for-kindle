#include <doctest/doctest.h>

#include "docs/engine_factory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

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

namespace {

// A one-page PDF whose page is a single dark gray RGB image of side x side pixels, run-length
// encoded (small on disk, side*side*3 bytes once decoded).
std::string heavyPdf(int side) {
    std::string img;
    size_t left = static_cast<size_t>(side) * side * 3;
    while (left) {
        const size_t n = std::min<size_t>(left, 128);
        img += static_cast<char>(257 - n);
        img += static_cast<char>(40);
        left -= n;
    }
    img += static_cast<char>(128);
    const std::string content = "q 612 0 0 792 0 0 cm /Im0 Do Q";
    std::vector<std::string> objs = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /XObject << /Im0 5 0 R >> >> "
        "/Contents 4 0 R >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /XObject /Subtype /Image /Width " + std::to_string(side) + " /Height " + std::to_string(side) +
            " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /RunLengthDecode /Length " +
            std::to_string(img.size()) + " >>\nstream\n" + img + "\nendstream",
    };
    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> at;
    for (size_t i = 0; i < objs.size(); ++i) {
        at.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t o : at) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        pdf += line;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}

}  // namespace

TEST_CASE("pdf engine: a small resource store; text-only engines a smaller one") {
    UserError e;
    auto reader = loadEngine(DocType::Pdf, EngineEnv{}, &e);
    REQUIRE(reader);
    CHECK(reader->cacheLimitBytes() == 24u << 20);
    REQUIRE(reader->open("tests/data/pdf/sample.pdf", &e));
    CHECK(reader->cacheLimitBytes() == 24u << 20);

    auto text = loadEngine(DocType::Pdf, EngineEnv{}, &e);
    REQUIRE(text);
    REQUIRE(text->openForText("tests/data/pdf/sample.pdf", &e));
    CHECK(text->cacheLimitBytes() == 8u << 20);
    std::string out;
    REQUIRE(text->fullText(out, &e));
    CHECK(out.find("Hamiltonian") != std::string::npos);
}

TEST_CASE("pdf engine: a page whose image is larger than the store still renders, also after a trim") {
    // 3000x3000 RGB: 27 MB decoded, over the reader's 24 MB store.
    const std::string path = "/tmp/zk-heavy-page.pdf";
    {
        std::ofstream f(path, std::ios::binary);
        f << heavyPdf(3000);
    }
    UserError e;
    auto engine = loadEngine(DocType::Pdf, EngineEnv{}, &e);
    REQUIRE(engine);
    engine->setLayout(Layout{});
    REQUIRE_MESSAGE(engine->open(path, &e), e.details);
    auto darkShare = [](const PageImage& p) {
        size_t dark = 0;
        for (uint8_t g : p.gray) dark += g < 100;
        return p.gray.empty() ? 0.0 : double(dark) / p.gray.size();
    };
    const PageImage first = engine->render(0);
    REQUIRE(first.width > 0);
    CHECK(darkShare(first) > 0.5);
    engine->trimCaches();
    const PageImage again = engine->render(0);
    CHECK(again.gray == first.gray);
    std::remove(path.c_str());
}
