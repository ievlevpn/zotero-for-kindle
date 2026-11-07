// Fast Font (Born2Root, MIT): bold word beginnings for faster reading, an OpenType "calt"
// feature that only shapes with HarfBuzz. Bundled in assets/fonts-fast (zotero/share/fonts).

#include <doctest/doctest.h>

#include "docs/engine_factory.h"
#include "docs/fonts.h"

#include <algorithm>
#include <filesystem>

using namespace zk;

namespace {

EngineEnv env() { return {"third_party/crengine/cr3gui/data", readingFontDirs(), "/tmp/zk-cre-cache-fast"}; }

// Share of dark pixels on page 8 of Alice in `font`.
double ink(DocumentEngine& epub, const std::string& font) {
    Layout l;
    l.width = 1236;
    l.height = 1487;
    l.font = font;
    epub.setLayout(l);
    const PageImage img = epub.render(8);
    const auto dark = std::count_if(img.gray.begin(), img.gray.end(), [](uint8_t g) { return g < 100; });
    return static_cast<double>(dark) / static_cast<double>(img.gray.size());
}

}  // namespace

TEST_CASE("fast fonts: Fast Serif, Fast Sans and Fast Mono are offered") {
    const auto fonts = readingFonts(readingFontDirs());
    for (const char* f : {"Fast Serif", "Fast Sans", "Fast Mono"}) {
        INFO(f);
        CHECK(std::find(fonts.begin(), fonts.end(), f) != fonts.end());
    }
}

TEST_CASE("fast fonts: word beginnings are bold (the font's calt feature is shaped)") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    UserError err;
    auto epub = loadEngine(DocType::Epub, env(), &err);
    REQUIRE(epub);
    REQUIRE(epub->open("tests/data/epub/alice-gutenberg-epub3.epub", &err));
    const double plain = ink(*epub, "Bookerly");
    const double serif = ink(*epub, "Fast Serif"), sans = ink(*epub, "Fast Sans"), mono = ink(*epub, "Fast Mono");
    MESSAGE("ink: Bookerly ", plain, ", Fast Serif ", serif, ", Fast Sans ", sans, ", Fast Mono ", mono);
    // Measured without shaping (the bold forms unused): 0.099, 0.078, 0.059; shaped: 0.119, 0.100,
    // 0.077. Midway between:
    CHECK(serif > 0.109);
    CHECK(sans > 0.089);
    CHECK(mono > 0.068);
    CHECK(ink(*epub, "Bookerly") == doctest::Approx(plain));  // and back: other fonts as before
}

#include <atomic>
#include <thread>

TEST_CASE("fast fonts: two books on two threads, one switching shaping, don't corrupt each other") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    // A book closing on its worker thread while the next one opens with another font: crengine's
    // fonts are process-wide, and switching shaping clears them.
    std::atomic<bool> stop{false};
    std::atomic<int> renders{0};
    std::thread other([&] {
        UserError err;
        auto b = loadEngine(DocType::Epub, env(), &err);
        REQUIRE(b);
        REQUIRE(b->open("tests/data/epub/frankenstein-epub2-ncx.epub", &err));
        Layout l;
        l.width = 1236;
        l.height = 1487;
        b->setLayout(l);
        for (int p = 2; !stop; p = p % 30 + 2) {
            const PageImage img = b->render(p);
            CHECK(img.width == 1236);
            ++renders;
        }
    });
    UserError err;
    auto a = loadEngine(DocType::Epub, env(), &err);
    REQUIRE(a);
    REQUIRE(a->open("tests/data/epub/alice-gutenberg-epub3.epub", &err));
    while (renders == 0) std::this_thread::yield();  // both books open and rendering
    for (int i = 0; i < 24; ++i) {
        Layout l;
        l.width = 1236;
        l.height = 1487;
        l.font = i % 2 ? "Fast Serif" : "Bookerly";
        a->setLayout(l);
        CHECK(a->render(3).width == 1236);
    }
    stop = true;
    other.join();
}
