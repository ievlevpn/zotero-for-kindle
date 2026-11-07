#pragma once
// The on-screen keyboard. On the Kindle it is the system keyboard, driven over lipc (spike S3);
// its keys arrive as X key events. On the desktop it is the real keyboard.

#include <memory>
#include <string>

#include "core/geometry.h"
#include "platform/lipc.h"

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
Rect parseKeyboardBounds(const std::string& lipcValue, int windowTop);

}  // namespace zk
