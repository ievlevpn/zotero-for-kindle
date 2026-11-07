// Plan 6: where the system keyboard is, on any Kindle.

#include <doctest/doctest.h>

#include "platform/device_profile.h"
#include "platform/keyboard.h"
#include "ui/metrics.h"

using namespace zk;

TEST_CASE("keyboard: lipc bounds are moved into our window by its measured origin") {
    // Measured on the Scribe with the keyboard open: 0:1672:1860:808; our window starts at y 101.
    CHECK(parseKeyboardBounds("0:1672:1860:808", 101) == Rect{0, 1571, 1860, 808});
}

TEST_CASE("keyboard: with no bounds from lipc the fallback is the device's keyboard at the bottom") {
    CHECK(keyboardFallback(kPw5, profileFor("PaperWhite 5", 300)) == Rect{0, 980, 1236, 567});
    CHECK(keyboardFallback(kScribe, profileFor("Scribe", 300)) == Rect{0, 2379 - 808, 1860, 808});
}

TEST_CASE("keyboard: an unknown device falls back to the lower half") {
    Metrics m = kScribe;
    m.device = "Kindle 99";
    CHECK(keyboardFallback(m, profileFor("Kindle 99", 300)) == Rect{0, 2379 / 2, 1860, 2379 - 2379 / 2});
}
