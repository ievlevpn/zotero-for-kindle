#include <doctest/doctest.h>

#include <bit>
#include <tuple>

#include <filesystem>
#include <vector>

#include "platform/color.h"
#include "platform/display.h"
#include "ui/canvas.h"

using namespace zk;

namespace {

// A canvas of this size, white.
struct White : Canvas {
    White(int w, int h) : Canvas(w, h) {
        cairo_set_source_rgb(cr(), 1, 1, 1);
        cairo_paint(cr());
    }
};

void fill(Canvas& c, Rect r, double red, double green, double blue) {
    cairo_set_source_rgb(c.cr(), red, green, blue);
    cairo_rectangle(c.cr(), r.x, r.y, r.w, r.h);
    cairo_fill(c.cr());
    cairo_surface_flush(c.surface());
}

const uint32_t* pixels(const Canvas& c) {
    cairo_surface_flush(c.surface());
    return reinterpret_cast<const uint32_t*>(cairo_image_surface_get_data(c.surface()));
}
int stride(const Canvas& c) { return cairo_image_surface_get_stride(c.surface()); }

}  // namespace

TEST_CASE("color: a gray region is not colored") {
    Canvas c(100, 100);
    cairo_set_source_rgb(c.cr(), 0.5, 0.5, 0.5);
    cairo_paint(c.cr());
    fill(c, {0, 0, 50, 100}, 0, 0, 0);
    CHECK_FALSE(isColored(pixels(c), stride(c), {0, 0, 100, 100}));
}

TEST_CASE("color: one red pixel in 10,000 is not colored; 2% red is") {
    White c(100, 100);
    fill(c, {10, 10, 1, 1}, 1, 0, 0);
    CHECK_FALSE(isColored(pixels(c), stride(c), {0, 0, 100, 100}));
    fill(c, {0, 50, 100, 2}, 1, 0, 0);  // 200 of 10,000
    CHECK(isColored(pixels(c), stride(c), {0, 0, 100, 100}));
    CHECK_FALSE(isColored(pixels(c), stride(c), {0, 0, 100, 40}));  // only what `r` covers counts
}

TEST_CASE("color: the threshold is more than 0.5% of pixels with chroma above 24") {
    White c(100, 100);
    fill(c, {0, 0, 50, 1}, 1, 0, 0);  // 50 of 10,000: exactly 0.5%
    CHECK_FALSE(isColored(pixels(c), stride(c), {0, 0, 100, 100}));
    fill(c, {50, 0, 1, 1}, 1, 0, 0);  // 51
    CHECK(isColored(pixels(c), stride(c), {0, 0, 100, 100}));
    White pale(10, 10);
    fill(pale, {0, 0, 10, 10}, 124 / 255.0, 100 / 255.0, 100 / 255.0);  // chroma 24: not colored
    CHECK_FALSE(isColored(pixels(pale), stride(pale), {0, 0, 10, 10}));
    fill(pale, {0, 0, 10, 10}, 125 / 255.0, 100 / 255.0, 100 / 255.0);  // 25
    CHECK(isColored(pixels(pale), stride(pale), {0, 0, 10, 10}));
}

TEST_CASE("color: waveforms on a color panel") {
    CHECK(colorWaveform(Waveform::Ui, false) == ColorWfm::Gl16);
    CHECK(colorWaveform(Waveform::Ui, true) == ColorWfm::Glrc16);
    CHECK(colorWaveform(Waveform::Quality, false) == ColorWfm::Gc16);
    CHECK(colorWaveform(Waveform::Quality, true) == ColorWfm::Gcc16);
    CHECK(colorWaveform(Waveform::Full, false) == ColorWfm::Gc16);
    CHECK(colorWaveform(Waveform::Full, true) == ColorWfm::Gcc16);
    for (bool colored : {false, true}) {
        CHECK(colorWaveform(Waveform::Fast, colored) == ColorWfm::Keep);
        CHECK(colorWaveform(Waveform::Ink, colored) == ColorWfm::Keep);
    }
    CHECK(std::string(colorWfmName(ColorWfm::Glrc16)) == "GLRC16");
    CHECK(std::string(colorWfmName(ColorWfm::Gcc16)) == "GCC16");
}

TEST_CASE("color: toXrgb writes the visual's masks and says whether the region is colored") {
    White c(8, 4);
    fill(c, {0, 0, 4, 4}, 1, 0, 0);
    fill(c, {4, 0, 4, 4}, 0, 1, 0);
    std::vector<uint32_t> dst(8 * 4, 0xdeadbeef);
    auto bytes = reinterpret_cast<uint8_t*>(dst.data());
    CHECK(toXrgb(c, bytes, 8 * 4, {0, 0, 8, 4}, 0xff0000, 0x00ff00, 0x0000ff));
    CHECK(dst[0] == 0xff0000);
    CHECK(dst[4] == 0x00ff00);
    // BGR: red is the low byte.
    CHECK(toXrgb(c, bytes, 8 * 4, {0, 0, 8, 4}, 0x0000ff, 0x00ff00, 0xff0000));
    CHECK(dst[0] == 0x0000ff);
    CHECK(dst[4] == 0x00ff00);
    // Narrower channels (RGB565 in 32 bits): full intensity fills the mask.
    toXrgb(c, bytes, 8 * 4, {0, 0, 8, 4}, 0xf800, 0x07e0, 0x001f);
    CHECK(dst[0] == 0xf800);
    CHECK(dst[4] == 0x07e0);
    // Only `r` is written.
    std::fill(dst.begin(), dst.end(), 0xdeadbeef);
    White plain(8, 4);
    CHECK_FALSE(toXrgb(plain, bytes, 8 * 4, {1, 1, 2, 2}, 0xff0000, 0x00ff00, 0x0000ff));
    CHECK(dst[0] == 0xdeadbeef);
    CHECK(dst[1 * 8 + 1] == 0xffffff);
    CHECK(dst[3 * 8 + 3] == 0xdeadbeef);
}

TEST_CASE("color: whole-byte masks take every value exactly, and the colored count stops at 0.5%") {
    // Every byte value in every channel: a 256 x 3 canvas, row 0 red ramps, row 1 green, row 2 blue.
    White c(256, 3);
    cairo_surface_flush(c.surface());
    auto* data = cairo_image_surface_get_data(c.surface());
    const int stride = cairo_image_surface_get_stride(c.surface());
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 256; ++x)
            reinterpret_cast<uint32_t*>(data + y * stride)[x] = static_cast<uint32_t>(x) << (16 - 8 * y) | 0x404040u;
    cairo_surface_mark_dirty(c.surface());
    std::vector<uint32_t> dst(256 * 3);
    for (const auto& [rm, gm, bm] : {std::tuple{0xff0000u, 0x00ff00u, 0x0000ffu}, std::tuple{0x0000ffu, 0x00ff00u, 0xff0000u},
                                     std::tuple{0xff000000u, 0x00ff0000u, 0x0000ff00u}}) {
        toXrgb(c, reinterpret_cast<uint8_t*>(dst.data()), 256 * 4, {0, 0, 256, 3}, rm, gm, bm);
        auto place = [](uint32_t v, uint32_t mask) { return v << std::countr_zero(mask); };
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 256; ++x) {
                const uint32_t p = reinterpret_cast<uint32_t*>(data + y * stride)[x];
                const uint32_t want = place((p >> 16) & 0xff, rm) | place((p >> 8) & 0xff, gm) | place(p & 0xff, bm);
                REQUIRE(dst[y * 256 + x] == want);
            }
    }
    // 0.5% of 200 x 100 is 100 pixels: 100 colored aren't enough, 101 are, wherever they are.
    for (int n : {100, 101}) {
        White w(200, 100);
        fill(w, {0, 99, 100, 1}, 1, 0, 0);       // the last row: 100 red
        if (n == 101) fill(w, {150, 0, 1, 1}, 0, 0, 1);  // and one blue at the top
        CHECK(toXrgb(w, reinterpret_cast<uint8_t*>(std::vector<uint32_t>(200 * 100).data()), 200 * 4, {0, 0, 200, 100}, 0xff0000,
                     0x00ff00, 0x0000ff) == (n == 101));
    }
}

TEST_CASE("color: toXgray writes toGray's luma into every channel") {
    White c(4, 1);
    fill(c, {0, 0, 1, 1}, 1, 0, 0);
    std::vector<uint8_t> gray(4);
    c.toGray(gray.data(), 4, {0, 0, 4, 1});
    std::vector<uint32_t> dst(4);
    toXgray(c, reinterpret_cast<uint8_t*>(dst.data()), 16, {0, 0, 4, 1}, 0xff0000, 0x00ff00, 0x0000ff);
    const uint32_t g = gray[0];
    CHECK(dst[0] == ((g << 16) | (g << 8) | g));
    CHECK(dst[1] == 0xffffff);
}

TEST_CASE("color: packRgb packs a region as R, G, B bytes") {
    const uint32_t px[2 * 3] = {0x112233, 0x445566, 0x778899, 0xaabbcc, 0xddeeff, 0x000000};
    uint8_t out[2 * 2 * 3];
    packRgb(px, 2 * 4, {0, 1, 2, 2}, out);
    const uint8_t want[] = {0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0, 0, 0};
    CHECK(std::equal(std::begin(out), std::end(out), std::begin(want)));
}

TEST_CASE("color: usable masks") {
    CHECK(usableMasks(0xff0000, 0x00ff00, 0x0000ff));
    CHECK(usableMasks(0x0000ff, 0x00ff00, 0xff0000));
    CHECK_FALSE(usableMasks(0, 0, 0));                         // a gray visual
    CHECK_FALSE(usableMasks(0x3ff00000, 0x000ffc00, 0x3ff));   // 10 bits a channel
    CHECK_FALSE(usableMasks(0xff0000, 0xff0000, 0x0000ff));    // overlapping
    CHECK_FALSE(usableMasks(0xf0f000, 0x000f00, 0x0000ff));    // not contiguous
}

TEST_CASE("color: caps from the panel, the setting and ZK_COLOR") {
    auto same = [](ColorCaps a, ColorCaps b) { return a.panel == b.panel && a.on == b.on; };
    CHECK(same(decideColorCaps(false, true, nullptr), {false, false}));  // every gray Kindle
    CHECK(same(decideColorCaps(false, false, nullptr), {false, false}));
    CHECK(same(decideColorCaps(true, true, nullptr), {true, true}));
    CHECK(same(decideColorCaps(true, false, nullptr), {true, false}));  // Display → Color off
    CHECK(same(decideColorCaps(true, true, "0"), {false, false}));      // ZK_COLOR=0: a gray Kindle
    CHECK(same(decideColorCaps(true, false, "0"), {false, false}));
    CHECK(same(decideColorCaps(false, true, "1"), {true, true}));       // ZK_COLOR=1 imitates a color panel
    CHECK(same(decideColorCaps(false, false, "1"), {true, false}));     // whose setting still counts
    CHECK(same(decideColorCaps(true, true, ""), {true, true}));         // anything else: as found
    CHECK(same(decideColorCaps(false, true, "yes"), {false, false}));
}

TEST_CASE("color: the path — X when it can, else the framebuffer, else gray; ZK_COLOR_PATH forces one that can") {
    const ColorCaps on{true, true}, off{true, false};
    CHECK(chooseColorPath(off, true, true, nullptr) == ColorPath::Gray);
    CHECK(chooseColorPath(off, true, true, "fb") == ColorPath::Gray);  // color off: nothing forces it
    CHECK(chooseColorPath(on, true, true, nullptr) == ColorPath::X);
    CHECK(chooseColorPath(on, false, true, nullptr) == ColorPath::Fb);
    CHECK(chooseColorPath(on, false, false, nullptr) == ColorPath::Gray);
    CHECK(chooseColorPath(on, true, true, "fb") == ColorPath::Fb);
    CHECK(chooseColorPath(on, false, true, "x") == ColorPath::Fb);  // X can't: the framebuffer's
    CHECK(chooseColorPath(on, true, false, "fb") == ColorPath::X);
    CHECK(chooseColorPath(on, true, true, "x") == ColorPath::X);
    CHECK(std::string(colorPathName(ColorPath::Fb)) == "fb");
}

// ---- The setting, the device profiles, the device test ----

#include "app/color_test.h"
#include "app/device_settings.h"
#include "app/settings_store.h"
#include "ui/text.h"
#include "fake_display.h"
#include "golden.h"
#include "platform/device_profile.h"
#include "store/db.h"

TEST_CASE("color: Display → Color is the device's setting, on by default") {
    CHECK(isDeviceKey("colorScreen"));
    DeviceSettings dev("");
    Db db{Db::open(":memory:")};
    Library lib{db};
    AppSettings s = loadSettings(lib, dev);
    CHECK(s.colorScreen);
    s.colorScreen = false;
    saveSettings(lib, dev, s);
    CHECK(dev.get("colorScreen") == "0");
    CHECK_FALSE(loadSettings(lib, dev).colorScreen);
}

TEST_CASE("color: the Colorsofts are 300 ppi screens") {
    for (const char* name : {"ColorSoft", "Scribe ColorSoft", "ColorSoft 2"}) {
        CHECK_MESSAGE(profileFor(name, 0).ppi == 300, name);
        CHECK_MESSAGE(profileFor(name, 150).ppi == 300, name);  // not the color layer's resolution
    }
    CHECK(profileFor("PaperWhite 4", 300).ppi == 300);
    CHECK(profileFor("Kindle 10", 167).ppi == 167);
}

namespace {

// A display with both color paths, recording what the device test asks of it.
struct ColorTestDisplay : test::FakeDisplay {
    struct Shown {
        ColorPath path;
        ColorWfm wfm;
    };
    std::vector<ColorPath> paths{ColorPath::X, ColorPath::Fb};
    std::vector<Shown> shown;
    std::vector<ColorPath> colorPaths() const override { return paths; }
    bool presentWith(const Canvas&, Rect, ColorPath p, ColorWfm w) override {
        shown.push_back({p, w});
        return true;
    }
};

}  // namespace

TEST_CASE("color: the device test shows the card through every path with every waveform, 3 s each") {
    ColorTestDisplay d;
    std::vector<int> waits;
    CHECK(runColorTest(d, [&](int ms) { waits.push_back(ms); }) == 0);
    // First today's gray, then each path with GL16, GLRC16, GC16, GCC16, AUTO.
    const ColorWfm wfms[] = {ColorWfm::Gl16, ColorWfm::Glrc16, ColorWfm::Gc16, ColorWfm::Gcc16, ColorWfm::Auto};
    REQUIRE(d.shown.size() == 11);
    CHECK(d.shown[0].path == ColorPath::Gray);
    CHECK(d.shown[0].wfm == ColorWfm::Gc16);
    for (int p = 0; p < 2; ++p)
        for (int w = 0; w < 5; ++w) {
            CHECK(d.shown[1 + p * 5 + w].path == d.paths[p]);
            CHECK(d.shown[1 + p * 5 + w].wfm == wfms[w]);
        }
    CHECK(waits == std::vector<int>(11, 3000));
    // Before each step the display hears what happened to its window (covered, in the background).
    CHECK(d.timeouts == std::vector<int>(11, 0));
    // Each card comes after a flashing white screen, so no step inherits the last one's picture.
    REQUIRE(d.presents.size() == 11);
    for (const auto& p : d.presents) CHECK(p.waveform == Waveform::Full);
}

TEST_CASE("color: without a color path the device test shows gray once") {
    ColorTestDisplay d;
    d.paths.clear();
    CHECK(runColorTest(d, [](int) {}) == 0);
    REQUIRE(d.shown.size() == 1);
    CHECK(d.shown[0].path == ColorPath::Gray);
}

TEST_CASE("color: the test card has the color bars, a gray ramp and Zotero's colors") {
    Canvas c(1236, 1547);
    drawColorTestCard(c, "x · GCC16");
    cairo_surface_flush(c.surface());
    CHECK(isColored(pixels(c), stride(c), {0, 0, 1236, 1547}));
    const Rect bars = colorTestBarsRect(c);
    CHECK((pixels(c)[(bars.y + 10) * stride(c) / 4 + bars.x + 10] & 0xffffff) == 0xff0000);  // red first
    const Rect ramp = colorTestRampRect(c);
    const uint32_t left = pixels(c)[(ramp.y + ramp.h / 2) * stride(c) / 4 + ramp.x + 2] & 0xffffff;
    const uint32_t right = pixels(c)[(ramp.y + ramp.h / 2) * stride(c) / 4 + ramp.x + ramp.w - 3] & 0xffffff;
    CHECK(left == 0x000000);
    CHECK(right == 0xffffff);
}

TEST_CASE("color: the test card matches the reviewed golden") {
    if (!std::filesystem::exists("assets/fonts-device/Bookerly-Regular.ttf")) return;
    addFontDir("assets/fonts-device");
    Canvas c(1236, 1547);
    drawColorTestCard(c, "2/11  path x  waveform GL16");
    test::checkGolden(c, "tests/golden/color-test-card.png");
}

// ---- The framebuffer path writes past X's clipping: only over a window nothing covers ----

TEST_CASE("color: the framebuffer path writes only in front, fully unobscured, without the keyboard") {
    CHECK(fbColorAllowed({true, true, false}));
    CHECK_FALSE(fbColorAllowed({false, true, false}));  // the screensaver, a system dialog in front
    CHECK_FALSE(fbColorAllowed({true, false, false}));  // partly covered (the status-bar pulldown)
    CHECK_FALSE(fbColorAllowed({true, true, true}));    // the system keyboard over the window
    CHECK_FALSE(fbColorAllowed({false, false, true}));
}

TEST_CASE("color: the gate starts closed and asks for one repaint in color each time it opens again") {
    FbColorGate gate;
    CHECK_FALSE(gate.allowed());              // nothing known about the window yet
    CHECK(gate.setUnobscured(true));          // mapped and visible: open, repaint
    CHECK(gate.allowed());
    CHECK_FALSE(gate.setUnobscured(true));    // no change, no repaint
    CHECK_FALSE(gate.setKeyboardUp(true));    // closes
    CHECK_FALSE(gate.allowed());
    CHECK(gate.setKeyboardUp(false));         // the keyboard went: repaint in color
    CHECK_FALSE(gate.setForeground(false));   // the screensaver
    CHECK_FALSE(gate.setUnobscured(false));
    CHECK_FALSE(gate.setForeground(true));    // back in front, still covered: stays closed
    CHECK_FALSE(gate.allowed());
    CHECK(gate.setUnobscured(true));          // uncovered: open, repaint
    CHECK(gate.allowed());
}

TEST_CASE("color: the framebuffer path writes only colored regions, and only while the gate is open") {
    CHECK(fbWritesRegion(true, true));
    CHECK_FALSE(fbWritesRegion(false, true));   // the UI, a gray page: X's gray alone, as on a gray Kindle
    CHECK_FALSE(fbWritesRegion(true, false));   // covered or behind: X's gray (X clips it)
    CHECK_FALSE(fbWritesRegion(false, false));
}

TEST_CASE("color: after a failed framebuffer write the path is chosen without it, even when forced") {
    const ColorCaps on{true, true};
    CHECK(chooseColorPath(on, false, false, nullptr) == ColorPath::Gray);
    CHECK(chooseColorPath(on, false, false, "fb") == ColorPath::Gray);
    CHECK(chooseColorPath(on, true, false, "fb") == ColorPath::X);
}

TEST_CASE("color: a gray Kindle's device file gets no colorScreen key from saving other settings") {
    DeviceSettings dev("");
    Db db{Db::open(":memory:")};
    Library lib{db};
    AppSettings s = loadSettings(lib, dev);
    s.flashEvery = 3;
    saveSettings(lib, dev, s);
    CHECK_FALSE(dev.get("colorScreen"));
    s.colorScreen = false;
    saveSettings(lib, dev, s);
    s.colorScreen = true;
    saveSettings(lib, dev, s);
    CHECK(dev.get("colorScreen") == "1");  // once there, it is kept up to date
}
