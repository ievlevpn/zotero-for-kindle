// Plan 6: the window, device and ppi the app lays out from.

#include <doctest/doctest.h>

#include "platform/device_profile.h"
#include "ui/metrics.h"

using namespace zk;

TEST_CASE("metrics: defaults are the PW5") {
    CHECK(metrics().width == 1236);
    CHECK(metrics().height == 1547);
    CHECK(metrics().window() == Rect{0, 0, 1236, 1547});
}

TEST_CASE("metrics: ScopedMetrics sets and restores") {
    {
        ScopedMetrics s(kScribe);
        CHECK(metrics().width == 1860);
        CHECK(metrics().height == 2379);
        CHECK(metrics().device == "Scribe");
    }
    CHECK(metrics().width == 1236);
}

TEST_CASE("device profile: the Scribe and the PW5 are known, 300 ppi, with keyboard heights") {
    const DeviceProfile scribe = profileFor("Scribe", 300);
    CHECK(scribe.name == "Scribe");
    CHECK(scribe.ppi == 300);
    CHECK(scribe.keyboardHeightPx == 808);
    const DeviceProfile pw5 = profileFor("PaperWhite 5", 300);
    CHECK(pw5.keyboardHeightPx == 567);
}

TEST_CASE("device profile: an unknown Kindle takes FBInk's ppi, 300 when it reports none") {
    CHECK(profileFor("Kindle 99", 212).ppi == 212);
    CHECK(profileFor("Kindle 99", 212).keyboardHeightPx == 0);
    CHECK(profileFor("x", 0).ppi == 300);
    CHECK(profileFor("x", 0).name == "x");
}

#include "app/app.h"
#include "fake_display.h"
#include "fake_sync.h"
#include "store/db.h"
#include "store/library.h"
#include "ui/widgets.h"

namespace {

struct NoKeyboard : Keyboard {
    void show() override {}
    void hide() override {}
    Rect bounds() const override { return {}; }
};

struct ShownScreen : Screen {
    std::string name;
    std::vector<std::string>* shown;
    ShownScreen(std::string n, std::vector<std::string>* s) : name(std::move(n)), shown(s) {}
    void draw(Canvas&) override {}
    void onGesture(const Gesture&, Nav&) override {}
    void onShow(Nav&) override { shown->push_back(name); }
};

}  // namespace

TEST_CASE("app: on the Scribe the tab bar and toast sit at its bottom and span its width") {
    ScopedMetrics s(kScribe);
    zk::test::FakeDisplay display;
    display.w = 1860;
    display.h = 2379;
    zk::test::FakeSync sync;
    NoKeyboard keyboard;
    Db db = Db::open(":memory:");
    Library lib(db);
    int64_t now = 1000;
    App app(display, lib, sync, keyboard, [&] { return now; });
    std::vector<std::string> shown;
    for (int i = 0; i < 4; ++i) app.setRoot(i, std::make_unique<ShownScreen>("tab" + std::to_string(i), &shown));
    app.start();
    CHECK(app.tabBarRect() == Rect{0, 2379 - kTabBarH, 1860, kTabBarH});
    const Rect toast = app.toastRect();
    CHECK(std::abs(toast.x - (1860 - toast.x - toast.w)) <= 1);
    CHECK(toast.y + toast.h <= 2379 - kTabBarH);
    display.tap({1860 * 7 / 8, 2379 - 60}, now);
    for (int i = 0; i < 4; ++i) {
        app.step(0);
        now += 70;
    }
    CHECK(shown.back() == "tab3");
}
