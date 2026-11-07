#pragma once

#include "platform/display.h"

namespace zk {

// Chooses the refresh after the window is exposed again. A full flash only when we come
// back from being fully hidden (wake from the screensaver); otherwise (the top menu
// closing over part of us) a quality refresh of the exposed area, no flash.
class ExposePolicy {
public:
    void visibilityChanged(bool fullyObscured) {
        if (fullyObscured) wasHidden_ = true;
    }
    Waveform onExpose() {
        Waveform w = wasHidden_ ? Waveform::Full : Waveform::Quality;
        wasHidden_ = false;
        return w;
    }

private:
    bool wasHidden_ = false;
};

}  // namespace zk
