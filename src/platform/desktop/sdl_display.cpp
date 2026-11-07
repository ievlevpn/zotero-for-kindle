// Desktop display for development: an SDL window showing the 8-bit gray framebuffer at
// half size. Mouse = finger. Waveforms are logged, not simulated.
#include <SDL.h>

#include <chrono>

#include "core/log.h"
#include "platform/display.h"
#include "ui/metrics.h"

namespace zk {

namespace {

constexpr int kScale = 2;

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

class SdlDisplay final : public Display {
public:
    // The window of the Kindle being imitated (metrics(): --device pw5|scribe).
    SdlDisplay() : kW(metrics().width), kH(metrics().height) {
        SDL_Init(SDL_INIT_VIDEO);
        win_ = SDL_CreateWindow("Zotero for Kindle (desktop)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                kW / kScale, kH / kScale, 0);
        ren_ = SDL_CreateRenderer(win_, -1, 0);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
        gray_.assign((size_t)kW * kH, 0xff);
    }

    int kW, kH;
    std::string deviceName() const override { return metrics().device; }  // the Kindle imitated
    // Turning swaps the window's sides, as the Kindle's window manager does.
    void setOrientation(Orientation o) override {
        if (o == orientation_) return;
        orientation_ = o;
        std::swap(kW, kH);
        SDL_SetWindowSize(win_, kW / kScale, kH / kScale);
        SDL_DestroyTexture(tex_);
        tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, kW, kH);
        gray_.assign((size_t)kW * kH, 0xff);
        resized_ = true;
    }
    Orientation orientation_ = Orientation::Portrait;
    bool resized_ = false;

    ~SdlDisplay() override {
        SDL_DestroyTexture(tex_);
        SDL_DestroyRenderer(ren_);
        SDL_DestroyWindow(win_);
        SDL_Quit();
    }

    int width() const override { return kW; }
    int height() const override { return kH; }

    void present(const Canvas& canvas, Rect r, Waveform w) override {
        r = r.intersected({0, 0, kW, kH});
        if (r.empty()) return;
        canvas.toGray(gray_.data(), kW, r);
        log(LogLevel::Debug, "present %d,%d %dx%d %s", r.x, r.y, r.w, r.h,
            w == Waveform::Fast ? "fast" : w == Waveform::Ui ? "ui" : w == Waveform::Quality ? "quality" : "full");
        upload();
    }

    bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) override {
        if (resized_) {
            resized_ = false;
            InputEvent e;
            e.kind = InputEvent::Kind::Resize;
            out.push_back(e);
        }
        SDL_Event ev;
        if (!SDL_WaitEventTimeout(&ev, timeout_ms)) return true;
        do {
            switch (ev.type) {
                case SDL_QUIT: return false;
                case SDL_MOUSEBUTTONDOWN:
                    down_ = true;
                    touch(out, {TouchEvent::Type::Down, 0, {ev.button.x * kScale, ev.button.y * kScale}, nowMs()});
                    break;
                case SDL_MOUSEMOTION:
                    if (down_)
                        touch(out, {TouchEvent::Type::Move, 0, {ev.motion.x * kScale, ev.motion.y * kScale}, nowMs()});
                    break;
                case SDL_MOUSEBUTTONUP:
                    down_ = false;
                    touch(out, {TouchEvent::Type::Up, 0, {ev.button.x * kScale, ev.button.y * kScale}, nowMs()});
                    break;
                case SDL_TEXTINPUT: key(out, KeyEvent::Code::Text, ev.text.text); break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_BACKSPACE) key(out, KeyEvent::Code::Backspace, "");
                    if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) key(out, KeyEvent::Code::Return, "");
                    if (ev.key.keysym.sym == SDLK_ESCAPE) key(out, KeyEvent::Code::Escape, "");
                    break;
                case SDL_WINDOWEVENT: upload(); break;
                default: break;
            }
        } while (SDL_PollEvent(&ev));
        return true;
    }

private:
    static void touch(std::vector<InputEvent>& out, TouchEvent t) {
        InputEvent e;
        e.touch = t;
        out.push_back(e);
    }
    static void key(std::vector<InputEvent>& out, KeyEvent::Code code, const char* text) {
        InputEvent e;
        e.kind = InputEvent::Kind::Key;
        e.key = {code, text};
        out.push_back(e);
    }

    void upload() {
        void* pixels;
        int pitch;
        SDL_LockTexture(tex_, nullptr, &pixels, &pitch);
        for (int y = 0; y < kH; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
            for (int x = 0; x < kW; ++x) {
                uint32_t g = gray_[(size_t)y * kW + x];
                row[x] = (g << 16) | (g << 8) | g;
            }
        }
        SDL_UnlockTexture(tex_);
        SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
        SDL_RenderPresent(ren_);
    }

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    std::vector<uint8_t> gray_;
    bool down_ = false;
};

}  // namespace

std::unique_ptr<Display> makeDisplay() { return std::make_unique<SdlDisplay>(); }

const char* fontDir() { return "assets/fonts-device"; }

}  // namespace zk
