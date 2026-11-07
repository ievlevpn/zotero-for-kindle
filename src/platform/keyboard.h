#pragma once
// The on-screen keyboard. On the Kindle it is the system keyboard, driven over lipc (spike S3);
// its keys arrive as X key events. On the desktop it is the real keyboard.

#include <memory>
#include <string>

#include "core/geometry.h"
#include "platform/device_profile.h"
#include "platform/lipc.h"
#include "ui/metrics.h"

namespace zk {

class Keyboard {
public:
    virtual ~Keyboard() = default;
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual Rect bounds() const = 0;  // window coordinates; empty while hidden
};

std::unique_ptr<Keyboard> makeKeyboard(Lipc&);

// lipc "x:y:w:h" in screen coordinates -> window rect (window top at `windowTop`); empty if malformed.
// `scale`: screen pixels per app pixel when the screen isn't 300 ppi (ScaledDisplay).
Rect parseKeyboardBounds(const std::string& lipcValue, int windowTop, double scale = 1.0);

// Where the keyboard is when lipc doesn't say: the device's keyboard height at the window's
// bottom, full width; the lower half on a device whose keyboard we haven't measured.
Rect keyboardFallback(const Metrics&, const DeviceProfile&);

}  // namespace zk
