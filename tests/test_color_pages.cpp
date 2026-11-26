// Color on Kindle Colorsoft, Task 2 (spec 2026-11-22-colorsoft-design.md §Design 4–7): pages,
// pictures and highlights in color when ColorCaps.on, and the gray pages of today when not.

#include <doctest/doctest.h>

#include "test_paths.h"
#include "docs/engine_factory.h"
#include "docs/render_worker.h"
#include "fake_engine.h"
#include "fake_nav.h"
#include "fixture_library.h"
#include "golden.h"
#include "platform/color.h"
#include "screens/annotation_bar.h"
#include "screens/book_notes.h"
#include "screens/reader.h"
#include "store/db.h"
#include "ui/canvas.h"
#include "ui/text.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace zk;
using namespace zk::test;

namespace {

uint64_t fnv(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) h = (h ^ b) * 1099511628211ull;
    return h;
}

EngineEnv epubEnv() { return {"third_party/crengine/cr3gui/data", {"assets/fonts-device"}, test::creCacheDir()}; }

std::unique_ptr<DocumentEngine> openDoc(DocType t, const std::string& path) {
    UserError e;
    auto engine = loadEngine(t, t == DocType::Epub ? epubEnv() : EngineEnv{}, &e);
    REQUIRE_MESSAGE(engine, e.details);
    REQUIRE(engine->open(path, &e));
    return engine;
}

// Pixels of an RGB page near `want` (each channel within 40).
int pixelsNear(const PageImage& p, uint32_t want) {
    int n = 0;
    for (size_t i = 0; i + 2 < p.rgb.size(); i += 3) {
        const int r = p.rgb[i], g = p.rgb[i + 1], b = p.rgb[i + 2];
        n += std::abs(r - int(want >> 16)) < 40 && std::abs(g - int((want >> 8) & 0xff)) < 40 && std::abs(b - int(want & 0xff)) < 40;
    }
    return n;
}

// Pixels of a canvas with chroma (max - min of R, G, B) above 24, in `r`.
int chromaPixels(Canvas& c, Rect r) {
    cairo_surface_flush(c.surface());
    const unsigned char* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            const uint32_t px = reinterpret_cast<const uint32_t*>(data + y * stride)[x];
            const int R = (px >> 16) & 0xff, G = (px >> 8) & 0xff, B = px & 0xff;
            n += std::max({R, G, B}) - std::min({R, G, B}) > 24;
        }
    return n;
}

uint32_t pixelAt(Canvas& c, Point p) {
    cairo_surface_flush(c.surface());
    const unsigned char* data = cairo_image_surface_get_data(c.surface());
    return reinterpret_cast<const uint32_t*>(data + p.y * cairo_image_surface_get_stride(c.surface()))[p.x] & 0xffffff;
}

}  // namespace

// ---- Engines: RGB when asked, today's gray bytes when not ----

TEST_CASE("color pages: a PDF renders in RGB when color is on, today's gray bytes when off") {
    auto e = openDoc(DocType::Pdf, "tests/data/pdf/color.pdf");
    const PageImage today = e->render(0);
    CHECK(fnv(today.gray) == 7239493071201414902ull);  // recorded before color existed
    CHECK(today.rgb.empty());
    e->setColor(true);
    const PageImage color = e->render(0);
    CHECK(color.gray.empty());
    REQUIRE(color.rgb.size() == static_cast<size_t>(color.width) * color.height * 3);
    CHECK(pixelsNear(color, 0xe61a1a) > 20000);  // the red square
    CHECK(pixelsNear(color, 0x1a4de6) > 20000);  // the blue bar
    CHECK(pixelsNear(color, 0xffffff) > color.width * color.height / 2);  // paper stays white
    e->setColor(false);
    const PageImage back = e->render(0);
    CHECK(back.rgb.empty());
    CHECK(back.gray == today.gray);
    auto sample = openDoc(DocType::Pdf, "tests/data/pdf/sample.pdf");
    CHECK(fnv(sample->render(0).gray) == 17197394955913123013ull);
}

TEST_CASE("color pages: an EPUB picture renders in RGB when color is on, today's gray bytes when off") {
    auto e = openDoc(DocType::Epub, "tests/data/epub/color-image.epub");
    const PageImage today = e->render(0);
    CHECK(fnv(today.gray) == 2708819504585304921ull);  // recorded before color existed
    CHECK(today.rgb.empty());
    e->setColor(true);
    const PageImage color = e->render(0);
    CHECK(color.gray.empty());
    REQUIRE(color.rgb.size() == static_cast<size_t>(color.width) * color.height * 3);
    CHECK(pixelsNear(color, 0xdc1e1e) > 20000);  // the picture's red half
    CHECK(pixelsNear(color, 0x1e46dc) > 20000);  // and its blue half
    int ink = 0;  // the text is still black
    for (size_t i = 0; i + 2 < color.rgb.size(); i += 3) ink += color.rgb[i] < 60 && color.rgb[i + 1] < 60 && color.rgb[i + 2] < 60;
    CHECK(ink > 1000);
    e->setColor(false);
    CHECK(e->render(0).gray == today.gray);
}

// ---- Memory: a color page is three bytes a pixel; the worker keeps what fits ----

TEST_CASE("color pages: a color page counts three bytes a pixel, and fewer of them are kept") {
    PageImage gray;
    gray.width = 1236, gray.height = 1487;
    gray.gray.assign(1236 * 1487, 255);
    PageImage rgb = gray;
    rgb.gray.clear();
    rgb.rgb.assign(1236 * 1487 * 3, 255);
    CHECK(pageBytes(gray) == 1236u * 1487);
    CHECK(pageBytes(rgb) == 3u * 1236 * 1487);
    // Gray: this page, the next, the previous and the one after the next (on the Scribe too).
    CHECK(pagesToKeep(5, 100, pageBytes(gray), true) == std::vector<int>{5, 6, 4, 7});
    CHECK(pagesToKeep(5, 100, 1860u * 2319, true) == std::vector<int>{5, 6, 4, 7});
    CHECK(pagesToKeep(5, 100, 1860u * 2480, true) == std::vector<int>{5, 6, 4, 7});
    CHECK(pagesToKeep(0, 100, pageBytes(gray), true) == std::vector<int>{0, 1, 2});
    CHECK(pagesToKeep(99, 100, pageBytes(gray), true) == std::vector<int>{99, 98});
    CHECK(pagesToKeep(98, 100, pageBytes(gray), true) == std::vector<int>{98, 99, 97});
    CHECK(pageCacheBudget(1860u * 2480, true) == kGrayPageCacheBytes);
    // Color (a Colorsoft's page): this page and the next, within the same memory.
    CHECK(pagesToKeep(5, 100, pageBytes(rgb)) == std::vector<int>{5, 6});
    CHECK(3 * pageBytes(rgb) > kPageCacheBytes);
    CHECK(2 * pageBytes(rgb) <= kPageCacheBytes);
    // A Scribe ColorSoft's color page (about the whole gray budget): this page and the next still.
    const size_t scribeColor = size_t{3} * 1860 * 2319;
    CHECK(pagesToKeep(5, 100, scribeColor) == std::vector<int>{5, 6});
    CHECK(pageCacheBudget(scribeColor) == 2 * scribeColor);
    CHECK(pageCacheBudget(pageBytes(gray)) == kPageCacheBytes);
    CHECK(pagesToKeep(5, 100, kPageCacheBytes + 1) == std::vector<int>{5, 6});
}

namespace {

// A fake engine that renders its pages in a pinkish color when asked.
class ColorFake : public FakeEngine {
public:
    std::atomic<bool> color{false};
    void setColor(bool on) override { color = on; }
    PageImage render(int page) override {
        PageImage img = FakeEngine::render(page);
        if (!color) return img;
        img.gray.clear();
        img.rgb.resize(static_cast<size_t>(img.width) * img.height * 3);
        for (size_t i = 0; i < img.rgb.size(); i += 3) img.rgb[i] = 200, img.rgb[i + 1] = 100, img.rgb[i + 2] = 100;
        return img;
    }
};

struct WorkerRig {
    ColorFake* engine = nullptr;
    std::unique_ptr<RenderWorker> worker{std::make_unique<RenderWorker>([this](DocType) {
        auto e = std::make_unique<ColorFake>();
        engine = e.get();
        return std::unique_ptr<DocumentEngine>(std::move(e));
    })};
    std::optional<RenderEvent> waitPage(int timeoutMs = 2000) {
        for (int i = 0; i < timeoutMs / 2; ++i) {
            RenderEvent e;
            while (worker->poll(e))
                if (e.kind == RenderEvent::Kind::Page) return e;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::nullopt;
    }
    // Renders once the worker has gone quiet (its neighbours drawn).
    int settledRenders() {
        int last = -1;
        for (int i = 0; i < 100 && engine->renders != last; ++i) {
            last = engine->renders;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return engine->renders;
    }
};

}  // namespace

TEST_CASE("color pages: the worker opens in color, boosts the page, keeps fewer pages, and re-renders on a switch") {
    WorkerRig rig;
    rig.worker->open("book.epub", DocType::Epub, Layout{}, DocPosition{"pos:450"}, "", true, 25);
    auto page = rig.waitPage();
    REQUIRE(page);
    REQUIRE(page->image);
    CHECK(page->image->gray.empty());
    REQUIRE_FALSE(page->image->rgb.empty());
    CHECK(page->image->rgb[0] > 200);  // boosted away from gray
    CHECK(page->image->rgb[1] < 100);
    CHECK(rig.settledRenders() == 2);  // page 5 and 6: a third color page doesn't fit
    rig.worker->setColor(false, 25);
    page = rig.waitPage();
    REQUIRE(page);
    CHECK(page->page == 5);
    CHECK(page->image->rgb.empty());
    CHECK(page->image->gray[0] == 5);
    CHECK(rig.settledRenders() == 2 + 4);  // again, gray: 5, then 6, 4 and 7
}

TEST_CASE("color pages: a burst of turns and jumps with color pages never holds more than the budget") {
    WorkerRig rig;
    Layout scribe;  // a Scribe ColorSoft's page area: a color page is about the whole gray budget
    scribe.width = 1860;
    scribe.height = 2319;
    rig.worker->open("book.epub", DocType::Epub, scribe, DocPosition{"pos:450"}, "", true, 25);
    REQUIRE(rig.waitPage());
    rig.engine->delayMs = 15;
    for (int p : {6, 7, 30, 31, 32, 60, 8, 9, 80, 81}) {  // overtaken renders, jumps, turns
        rig.worker->show(p);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    rig.settledRenders();
    const size_t page = size_t{3} * 1860 * 2319;
    CHECK(rig.worker->peakCachedBytes() <= pageCacheBudget(page));
    CHECK(rig.worker->peakCachedBytes() >= 2 * page);  // the next page is kept ahead
}

TEST_CASE("color pages: a gray worker keeps four pages, two of them ahead") {
    WorkerRig rig;
    rig.worker->open("book.epub", DocType::Epub, Layout{}, DocPosition{"pos:450"});
    REQUIRE(rig.waitPage());
    CHECK(rig.settledRenders() == 4);
    CHECK_FALSE(rig.engine->color);
}

// ---- Highlight tints and the saturation boost ----

TEST_CASE("color pages: a highlight's tint is its Zotero color 45% over white") {
    CHECK(highlightTint("#ffd400") == 0xffec8cu);
    CHECK(highlightTint("#FFD400") == 0xffec8cu);
    CHECK(highlightTint("#2ea8e5") == 0xa1d8f3u);
    CHECK(highlightTint("") == 0xd9d9d9u);  // gray, as Zotero's #aaaaaa
    CHECK(highlightTint("blue") == 0xd9d9d9u);
    CHECK(zoteroRgb("#ff6666") == 0xff6666u);
    CHECK(zoteroRgb("#12") == 0xaaaaaau);
}

TEST_CASE("color pages: the saturation boost leaves grays alone and moves colors away from gray") {
    std::vector<uint8_t> px = {0, 0, 0, 128, 128, 128, 255, 255, 255, 200, 100, 100, 250, 20, 20};
    boostSaturation(px.data(), 5, 25);
    CHECK(std::vector<uint8_t>(px.begin(), px.begin() + 9) == std::vector<uint8_t>{0, 0, 0, 128, 128, 128, 255, 255, 255});
    CHECK(px[9] > 200);
    CHECK(px[10] < 100);
    CHECK(px[11] < 100);
    CHECK(px[10] == px[11]);
    CHECK(px[12] == 255);  // clamped
    CHECK(px[13] < 20);
    std::vector<uint8_t> same = {200, 100, 100};
    boostSaturation(same.data(), 1, 0);
    CHECK(same == std::vector<uint8_t>{200, 100, 100});
    CHECK(colorBoostPct(nullptr) == 25);
    CHECK(colorBoostPct("40") == 40);
    CHECK(colorBoostPct("0") == 0);
    CHECK(colorBoostPct("x") == 25);
    CHECK(colorBoostPct("500") == 100);
}

// ---- The reader: a PDF page with highlights in Zotero's colors ----

namespace {

struct ReaderRig {
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    std::unique_ptr<ReaderScreen> reader;
    explicit ReaderRig(ColorCaps caps) {
        ::unsetenv("ZK_COLOR_BOOST");  // the default boost the goldens were made with, whatever the shell has
        addFontDir("assets/fonts-device");
        fillFixtureLibrary(lib, 10);
        nav.caps = caps;
        // Over the text lines of color.pdf (baselines at 500, 472, … PDF points, 14 pt Helvetica).
        const char* colours[] = {"#ffd400", "#ff6666", "#5fb236", "#2ea8e5", "#a28ae5", "#f19837"};
        for (int i = 0; i < 6; ++i) {
            AnnotationRecord a;
            a.key = "COLANN0" + std::to_string(i);
            a.parentKey = "EP000003";
            a.type = i == 5 ? "underline" : "highlight";
            a.color = colours[i];
            const int y = 500 - 28 * i;
            a.position = "{\"pageIndex\":0,\"rects\":[[72," + std::to_string(y - 4) + ",500," + std::to_string(y + 12) + "]]}";
            a.sortIndex = "00000|00000" + std::to_string(i) + "|00000";
            lib.upsertAnnotation(a);
        }
        reader = std::make_unique<ReaderScreen>(*lib.rowFor("IT000003"), "tests/data/pdf/color.pdf", DocType::Pdf, [] {
            return std::make_unique<RenderWorker>([](DocType t) {
                UserError e;
                return loadEngine(t, EngineEnv{}, &e);
            });
        }, 6);
        nav.visible = reader.get();
        reader->onShow(nav);
    }
    bool settle(const std::function<bool()>& done) {
        for (int i = 0; i < 1000; ++i) {
            reader->onTick(nav);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    bool highlighted() { return reader->ready() && reader->highlights().size() >= 6; }
    Rect box(const std::string& key) {
        for (const auto& b : reader->highlights())
            if (b.annotationKey == key) return b.rect;
        return {};
    }
};

}  // namespace

TEST_CASE("color pages: with Color off on a color panel the reader draws today's gray page") {
    ReaderRig off({true, false});
    ReaderRig gray({});
    REQUIRE(off.settle([&] { return off.highlighted(); }));
    REQUIRE(gray.settle([&] { return gray.highlighted(); }));
    Canvas a(1236, 1547), b(1236, 1547);
    off.reader->draw(a);
    gray.reader->draw(b);
    CHECK(chromaPixels(a, {0, 0, 1236, 1547}) == 0);
    cairo_surface_flush(a.surface());
    cairo_surface_flush(b.surface());
    CHECK(std::memcmp(cairo_image_surface_get_data(a.surface()), cairo_image_surface_get_data(b.surface()),
                      static_cast<size_t>(cairo_image_surface_get_stride(a.surface())) * 1547) == 0);
}

TEST_CASE("color pages: on a color screen the page is in color and highlights in their Zotero colors") {
    ReaderRig rig({true, true});
    REQUIRE(rig.settle([&] { return rig.highlighted(); }));
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    CHECK(chromaPixels(c, ReaderScreen::pageArea()) > 40000);
    // Yellow highlight: paper under it is the tint (blue lowest), multiplied: ink stays dark.
    const Rect y = rig.box("COLANN00");
    REQUIRE_FALSE(y.empty());
    int tinted = 0;
    for (int x = y.x + 2; x < y.x + y.w - 2; ++x)
        for (int yy = y.y + 1; yy < y.y + y.h - 1; ++yy) tinted += pixelAt(c, {x, yy}) == highlightTint("#ffd400");
    CHECK(tinted > y.w * y.h / 4);
    // The orange underline: a bar in the full color.
    const Rect u = rig.box("COLANN05");
    REQUIRE_FALSE(u.empty());
    CHECK(pixelAt(c, {u.x + u.w / 2, u.y + u.h - 2}) == zoteroRgb("#f19837"));
}

TEST_CASE("color pages: turning Color off and on while a book is open renders the page again") {
    ReaderRig rig({true, true});
    REQUIRE(rig.settle([&] { return rig.highlighted(); }));
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    REQUIRE(chromaPixels(c, ReaderScreen::pageArea()) > 0);
    rig.nav.setColorScreen(false);
    REQUIRE(rig.settle([&] {
        rig.reader->draw(c);
        return chromaPixels(c, ReaderScreen::pageArea()) == 0;
    }));
    rig.nav.setColorScreen(true);
    REQUIRE(rig.settle([&] {
        rig.reader->draw(c);
        return chromaPixels(c, {72, 100, 400, 400}) > 10000;  // the red square back in red
    }));
}

TEST_CASE("color pages: a PDF page with color highlights matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    ReaderRig rig({true, true});
    REQUIRE(rig.settle([&] { return rig.highlighted(); }));
    Canvas c(1236, 1547);
    rig.reader->draw(c);
    test::checkGolden(c, "tests/golden/reader-color-highlights.png");
}

// ---- Accents: the annotation bar's dots and the notes' color marks ----

TEST_CASE("color pages: the annotation bar's dots are Zotero's colors on a color screen, shades otherwise") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    AnnotationBar bar(AnnotationBar::Mode::Create, {100, 900, 600, 50}, "#2ea8e5", true, TagRow({"method", "to-cite"}, {}));
    Canvas gray(1236, 1547), color(1236, 1547);
    for (Canvas* c : {&gray, &color}) {  // on white paper
        cairo_set_source_rgb(c->cr(), 1, 1, 1);
        cairo_paint(c->cr());
    }
    bar.draw(gray.cr());
    bar.draw(color.cr(), true);
    for (int i = 0; i < 8; ++i) {
        const Rect d = bar.dotRect(i);
        const Point centre{d.x + d.w / 2, d.y + 38};
        const uint32_t g = shadeOf(zoteroColours()[i].hex);
        CHECK(pixelAt(gray, centre) == (g << 16 | g << 8 | g));
        CHECK(pixelAt(color, centre) == zoteroRgb(zoteroColours()[i].hex));
    }
    test::checkGolden(color, "tests/golden/annotation-bar-color.png");
}

TEST_CASE("color pages: Book Notes marks each highlight with its Zotero color on a color screen") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Db db{Db::open(":memory:")};
    Library lib{db};
    FakeNav nav{lib};
    fillFixtureLibrary(lib, 10);
    const char* colours[] = {"#ff6666", "#5fb236", "#a28ae5"};
    int n = 0;
    for (auto a : lib.annotationsFor("EP000003")) {
        a.color = colours[n++ % 3];
        lib.upsertAnnotation(a);
    }
    REQUIRE(n == 3);
    BookNotesScreen gray(*lib.rowFor("IT000003"), nullptr);
    nav.visible = &gray;
    gray.onShow(nav);
    Canvas g(1236, 1547);
    gray.draw(g);
    CHECK(chromaPixels(g, {0, 0, 1236, 1547}) == 0);
    nav.caps = {true, true};
    BookNotesScreen notes(*lib.rowFor("IT000003"), nullptr);
    nav.visible = &notes;
    notes.onShow(nav);
    Canvas c(1236, 1547);
    notes.draw(c);
    const auto rows = notes.rowRects();
    REQUIRE(rows.size() == 3);
    std::vector<uint32_t> marks;
    for (const Rect& r : rows) marks.push_back(pixelAt(c, {r.x + 4, r.y + r.h / 2}));
    std::vector<uint32_t> want;
    for (const auto& a : lib.annotationsFor("EP000003")) want.push_back(zoteroRgb(a.color));
    std::sort(marks.begin(), marks.end());
    std::sort(want.begin(), want.end());
    CHECK(marks == want);
    test::checkGolden(c, "tests/golden/book-notes-color.png");
}
