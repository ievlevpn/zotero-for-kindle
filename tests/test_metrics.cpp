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
