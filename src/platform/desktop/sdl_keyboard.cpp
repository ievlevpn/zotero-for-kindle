#include "platform/keyboard.h"

#include <SDL.h>

namespace zk {

namespace {

// The desktop has a real keyboard; reserve the Kindle keyboard's area so layouts match.
class SdlKeyboard final : public Keyboard {
public:
    void show() override {
        SDL_StartTextInput();
        shown_ = true;
    }
    void hide() override {
        SDL_StopTextInput();
        shown_ = false;
    }
    Rect bounds() const override {
        return shown_ ? keyboardFallback(metrics(), profileFor(metrics().device, 300)) : Rect{};
    }

private:
    bool shown_ = false;
};

}  // namespace

std::unique_ptr<Keyboard> makeKeyboard(Lipc&) { return std::make_unique<SdlKeyboard>(); }

}  // namespace zk
