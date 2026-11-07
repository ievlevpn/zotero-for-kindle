#include <doctest/doctest.h>

#include "docs/engine_factory.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace zk;

namespace {

std::string exportedSymbols(const std::string& lib) {
#ifdef __APPLE__
    const std::string cmd = "nm -gU " + lib + " > /tmp/zk-nm.txt 2>/dev/null";
#else
    const std::string cmd = "nm -D --defined-only " + lib + " > /tmp/zk-nm.txt 2>/dev/null";
#endif
    if (std::system(cmd.c_str()) != 0) return "nm failed";
    std::ifstream in("/tmp/zk-nm.txt");
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

EngineEnv testEnv() {
    return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, "/tmp/zk-cre-cache"};
}

}  // namespace

TEST_CASE("engine isolation: the EPUB module exports only its factory") {
    const char* env = std::getenv("ZK_LIB_DIR");
    const std::string dir = env ? env : "build-desktop";
    const std::string syms = exportedSymbols(dir + "/libzk-epub.so");
    INFO(syms);
    int lines = 0, ours = 0;
    std::istringstream ls(syms);
    for (std::string line; std::getline(ls, line);) {
        if (line.find(" T ") == std::string::npos && line.find(" D ") == std::string::npos &&
            line.find(" B ") == std::string::npos && line.find(" S ") == std::string::npos)
            continue;  // only defined code/data symbols count
        ++lines;
        ours += line.find("zk_epub_engine_create") != std::string::npos;
    }
    CHECK(ours == 1);
    CHECK(lines == 1);
}

TEST_CASE("engine isolation: the EPUB module loads and renders a page") {
    UserError err;
    auto engine = loadEngine(DocType::Epub, testEnv(), &err);
    REQUIRE_MESSAGE(engine, err.details);
    REQUIRE(engine->open("tests/data/epub/pride-and-prejudice.epub", &err));
    CHECK(engine->pageCount() > 100);
    PageImage page = engine->render(10);
    REQUIRE(page.width == 1236);
    int ink = 0;
    for (uint8_t g : page.gray) ink += g < 100;
    CHECK(ink > 1000);
    PageImage first = engine->render(0);
    CHECK(first.gray != page.gray);  // the requested page, not always the first one
    CHECK(engine->render(10).gray == page.gray);
}

TEST_CASE("engine isolation: a missing module is a plain error") {
    UserError err;
    auto engine = loadEngine(DocType::Pdf, testEnv(), &err);
    if (!engine) CHECK(err.message == "The PDF reader is missing from this install.");
}
