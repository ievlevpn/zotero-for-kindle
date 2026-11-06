#include <doctest/doctest.h>

#include "platform/kindle/expose_policy.h"

using namespace zk;

TEST_CASE("expose: closing the top menu repaints without a flash") {
    ExposePolicy p;
    p.visibilityChanged(false);  // partially obscured by the menu
    CHECK(p.onExpose() == Waveform::Quality);
}

TEST_CASE("expose: waking from the screensaver flashes once") {
    ExposePolicy p;
    p.visibilityChanged(true);  // fully obscured: screensaver
    p.visibilityChanged(false);
    CHECK(p.onExpose() == Waveform::Full);
    CHECK(p.onExpose() == Waveform::Quality);  // later exposes don't flash again
}
