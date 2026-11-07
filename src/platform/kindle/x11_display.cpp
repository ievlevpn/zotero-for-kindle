// Kindle display: an X11 window under the awesome WM + explicit e-ink refresh via FBInk.
// See research/spikes/s2-s5-window-refresh.md.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fbink.h>
#include <poll.h>

#include <chrono>
#include <cstring>

#include "core/log.h"
#include "platform/display.h"
#include "platform/keysym.h"
#include "platform/kindle/expose_policy.h"
#include "platform/kindle/x_touch.h"

namespace zk {

namespace {

constexpr const char* kWindowTitle = "L:A_N:application_PC:T_ID:org.zotero.kindle_O:U";

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

class X11Display final : public Display {
public:
    X11Display() {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            log(LogLevel::Error, "cannot open X display");
            return;
        }
        int scr = DefaultScreen(dpy_);
        w_ = DisplayWidth(dpy_, scr);
        h_ = DisplayHeight(dpy_, scr);
        win_ = XCreateSimpleWindow(dpy_, RootWindow(dpy_, scr), 0, 0, w_, h_, 0, 0, WhitePixel(dpy_, scr));
        XStoreName(dpy_, win_, kWindowTitle);
        XSelectInput(dpy_, win_, ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                                     Button1MotionMask | FocusChangeMask | VisibilityChangeMask | KeyPressMask);
        XMapWindow(dpy_, win_);
        gc_ = XCreateGC(dpy_, win_, 0, nullptr);
        waitForFinalGeometry();
        backing_.assign((size_t)w_ * h_, 0xff);
        image_ = XCreateImage(dpy_, DefaultVisual(dpy_, scr), 8, ZPixmap, 0, reinterpret_cast<char*>(backing_.data()),
                              w_, h_, 8, w_);

        fb_ = fbink_open();
        std::memset(&cfg_, 0, sizeof cfg_);
        cfg_.is_quiet = true;
        if (fb_ < 0 || fbink_init(fb_, &cfg_) < 0) {
            log(LogLevel::Warn, "FBInk init failed: X refreshes only");
        } else {
            FBInkState state{};
            fbink_get_state(&cfg_, &state);
            deviceName_ = state.device_name;
            ppi_ = state.screen_dpi;
        }
        log(LogLevel::Info, "window %dx%d at (%d,%d)", w_, h_, originX_, originY_);
    }

    ~X11Display() override {
        if (image_) {
            image_->data = nullptr;
            XDestroyImage(image_);
        }
        if (fb_ >= 0) fbink_close(fb_);
        if (dpy_) XCloseDisplay(dpy_);
    }

    int width() const override { return w_; }
    int height() const override { return h_; }
    Point origin() const override { return {originX_, originY_}; }
    std::string deviceName() const override { return deviceName_; }
    int ppi() const override { return ppi_; }

    void present(const Canvas& canvas, Rect r, Waveform w) override {
        r = r.intersected({0, 0, w_, h_});
        if (r.empty() || !dpy_) return;
        canvas.toGray(backing_.data(), w_, r);
        putAndRefresh(r, w);
    }

    bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) override {
        if (!dpy_) return false;
        if (!XPending(dpy_)) {
            pollfd p{ConnectionNumber(dpy_), POLLIN, 0};
            poll(&p, 1, timeout_ms);
        }
        while (XPending(dpy_)) {
            XEvent ev;
            XNextEvent(dpy_, &ev);
            switch (ev.type) {
                case ButtonPress:
                case ButtonRelease:
                case MotionNotify: {
                    XPointerEvent kind = ev.type == ButtonPress     ? XPointerEvent::Press
                                         : ev.type == ButtonRelease ? XPointerEvent::Release
                                                                    : XPointerEvent::Motion;
                    unsigned button = ev.type == MotionNotify ? 0 : ev.xbutton.button;
                    Point pos = ev.type == MotionNotify ? Point{ev.xmotion.x, ev.xmotion.y}
                                                        : Point{ev.xbutton.x, ev.xbutton.y};
                    if (auto t = touchFromX(kind, button, fingerDown_, pos, nowMs())) {
                        if (t->type == TouchEvent::Type::Down) fingerDown_ = true;
                        if (t->type == TouchEvent::Type::Up) fingerDown_ = false;
                        InputEvent e;
                        e.touch = *t;
                        out.push_back(e);
                    }
                    break;
                }
                case KeyPress:
                    if (auto k = keyFromKeysym(keysymOf(ev.xkey))) {
                        InputEvent e;
                        e.kind = InputEvent::Kind::Key;
                        e.key = *k;
                        out.push_back(e);
                    }
                    break;
                case VisibilityNotify:
                    expose_.visibilityChanged(ev.xvisibility.state == VisibilityFullyObscured);
                    if (ev.xvisibility.state == VisibilityFullyObscured) setBackground(true, out);
                    break;
                case Expose:
                    // Repaint the exposed area from the backing store: flash only after wake.
                    exposed_ = exposed_.united({ev.xexpose.x, ev.xexpose.y, ev.xexpose.width, ev.xexpose.height});
                    if (ev.xexpose.count == 0) {
                        putAndRefresh(exposed_.intersected({0, 0, w_, h_}), expose_.onExpose());
                        exposed_ = {};
                    }
                    setBackground(false, out);
                    break;
                case FocusOut:
                    log(LogLevel::Debug, "focus out (background)");
                    setBackground(true, out);
                    break;
                case FocusIn:
                    log(LogLevel::Debug, "focus in");
                    setBackground(false, out);
                    break;
                case DestroyNotify: return false;
                default: break;
            }
        }
        return true;
    }

private:
    // The text a key produces: XLookupString applies the keyboard's shift state; the Kindle's
    // keyboard sends ready-made keysyms (S3), so the unshifted keysym is the fallback.
    static unsigned long keysymOf(XKeyEvent& key) {
        KeySym sym = 0;
        char buf[8];
        XLookupString(&key, buf, sizeof buf, &sym, nullptr);
        return sym ? sym : XLookupKeysym(&key, 0);
    }

    void setBackground(bool background, std::vector<InputEvent>& out) {
        if (background == background_) return;
        background_ = background;
        InputEvent e;
        e.kind = background ? InputEvent::Kind::Background : InputEvent::Kind::Foreground;
        out.push_back(e);
    }

    void waitForFinalGeometry() {
        // The WM maps us full-screen, then resizes below the status bar (~350 ms).
        int64_t deadline = nowMs() + 2000;
        while (nowMs() < deadline) {
            pollfd p{ConnectionNumber(dpy_), POLLIN, 0};
            poll(&p, 1, 50);
            while (XPending(dpy_)) {
                XEvent ev;
                XNextEvent(dpy_, &ev);
                if (ev.type == ConfigureNotify) {
                    w_ = ev.xconfigure.width;
                    h_ = ev.xconfigure.height;
                }
            }
            if (h_ < DisplayHeight(dpy_, DefaultScreen(dpy_))) break;
        }
        Window child;
        XTranslateCoordinates(dpy_, win_, DefaultRootWindow(dpy_), 0, 0, &originX_, &originY_, &child);
    }

    void putAndRefresh(Rect r, Waveform w) {
        XPutImage(dpy_, win_, gc_, image_, r.x, r.y, r.x, r.y, r.w, r.h);
        XSync(dpy_, False);
        if (fb_ < 0) return;
        cfg_.wfm_mode = w == Waveform::Fast ? WFM_DU : w == Waveform::Ui ? WFM_GL16 : WFM_GC16;
        cfg_.is_flashing = w == Waveform::Full;
        fbink_refresh(fb_, r.y + originY_, r.x + originX_, r.w, r.h, &cfg_);
    }

    ::Display* dpy_ = nullptr;
    Window win_ = 0;
    GC gc_ = nullptr;
    XImage* image_ = nullptr;
    std::vector<uint8_t> backing_;
    int w_ = 0, h_ = 0, originX_ = 0, originY_ = 0;
    int fb_ = -1;
    bool fingerDown_ = false;
    ExposePolicy expose_;
    bool background_ = false;
    Rect exposed_;
    FBInkConfig cfg_;
    std::string deviceName_ = "Kindle";
    int ppi_ = 0;
};

}  // namespace

std::unique_ptr<Display> makeDisplay() { return std::make_unique<X11Display>(); }

const char* fontDir() { return "/usr/java/lib/fonts"; }

}  // namespace zk
