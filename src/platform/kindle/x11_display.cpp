// Kindle display: an X11 window under the awesome WM + explicit e-ink refresh via FBInk.
// See research/spikes/s2-s5-window-refresh.md.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <fbink.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/wake.h"
#include "platform/display.h"
#include "platform/keysym.h"
#include "platform/kindle/evdev_pen.h"
#include "platform/kindle/expose_policy.h"
#include "platform/kindle/wet_ink.h"
#include "platform/kindle/x_touch.h"

namespace zk {

namespace {

constexpr const char* kWindowTitle = "L:A_N:application_PC:T_ID:org.zotero.kindle_O:U";

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The self-pipe workers wake the loop through (core/wake.h): its read end is in pollEvents' poll.
int gWakePipe[2] = {-1, -1};

void writeWake() {
    if (gWakePipe[1] < 0) return;
    const char b = 1;
    [[maybe_unused]] const ssize_t n = ::write(gWakePipe[1], &b, 1);  // full (EAGAIN): a wake is waiting anyway
}

class X11Display final : public Display {
public:
    X11Display() {
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) {
            log(LogLevel::Error, "cannot open X display");
            return;
        }
        // FBInk first: the device it names decides the window (wet ink needs it full screen).
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
        int scr = DefaultScreen(dpy_);
        w_ = DisplayWidth(dpy_, scr);
        h_ = DisplayHeight(dpy_, scr);
        win_ = XCreateSimpleWindow(dpy_, RootWindow(dpy_, scr), 0, 0, w_, h_, 0, 0, WhitePixel(dpy_, scr));
        // Wet ink draws only over the top window: the status bar would be it, so go full screen.
        // Only where its protocol was checked (the 1st-gen Scribe); ZK_WET_INK=1 / ZK_NO_WET_INK=1.
        const char* forced = std::getenv("ZK_WET_INK");
        const bool wetInk = std::getenv("ZK_NO_WET_INK") == nullptr &&
                            (deviceName_ == "Scribe" || (forced && std::strcmp(forced, "1") == 0));
        if (wetInk) wet_ = WetInk::open();
        if (wet_) title_.replace(title_.find("PC:T"), 4, "PC:N");
        XStoreName(dpy_, win_, title_.c_str());
        XSelectInput(dpy_, win_, ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                                     Button1MotionMask | FocusChangeMask | VisibilityChangeMask | KeyPressMask);
        XMapWindow(dpy_, win_);
        gc_ = XCreateGC(dpy_, win_, 0, nullptr);
        waitForFinalGeometry();
        backing_.assign((size_t)w_ * h_, 0xff);
        image_ = XCreateImage(dpy_, DefaultVisual(dpy_, scr), 8, ZPixmap, 0, reinterpret_cast<char*>(backing_.data()),
                              w_, h_, 8, w_);

        {
            // The panel is portrait in its native orientation; the root window shows how it's turned now.
            const int rw = DisplayWidth(dpy_, scr), rh = DisplayHeight(dpy_, scr);
            pen_ = std::make_unique<EvdevPen>(std::min(rw, rh), std::max(rw, rh), wet_ != nullptr);
            updatePenView();
            if (wet_ && !wet_->connect(win_, std::min(rw, rh), std::max(rw, rh))) {
                // No wet ink after all: the status bar back, and the pen as the system re-sends it.
                wet_.reset();
                title_.replace(title_.find("PC:N"), 4, "PC:T");
                XStoreName(dpy_, win_, title_.c_str());
                XFlush(dpy_);
                pen_ = std::make_unique<EvdevPen>(std::min(rw, rh), std::max(rw, rh));
                updatePenView();
            }
        }
        if (gWakePipe[0] < 0 && ::pipe(gWakePipe) == 0) {
            for (int fd : gWakePipe) {
                ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            setUiWaker(writeWake);
        }
        log(LogLevel::Info, "window %dx%d at (%d,%d)", w_, h_, originX_, originY_);
    }

    // How the screen is turned from the digitizer. The raw digitizer (wet ink) stays with the
    // panel: the framebuffer's rotation says how far the picture is turned, upside down included
    // (the Scribe turns 180 degrees for the left hand without the window changing size).
    // fb0 rotate 3 is upright on the Scribe (2026-11-07: raw corners matched the screen).
    // Otherwise landscape reading is a quarter turn (clockwise assumed; ZK_PEN_TURNS overrides).
    void updatePenView() {
        if (!pen_) return;
        int turns = 0;
        if (wet_) {
            if (FILE* f = std::fopen("/sys/class/graphics/fb0/rotate", "r")) {
                int r = 3;
                if (std::fscanf(f, "%d", &r) == 1) turns = ((3 - r) % 4 + 4) % 4;
                std::fclose(f);
            }
        } else {
            const int scr = DefaultScreen(dpy_);
            turns = DisplayWidth(dpy_, scr) > DisplayHeight(dpy_, scr) ? 1 : 0;
        }
        if (turns != penTurns_) log(LogLevel::Info, "pen: screen turned %d quarter(s) from the digitizer", turns);
        penTurns_ = turns;
        pen_->setView(turns, {originX_, originY_});
    }

    ~X11Display() override {
        setUiWaker(nullptr);
        wet_.reset();  // leaves the handler off before our window goes
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
    // The window manager turns the screen for our window when the title's orientation field
    // changes (O:U upright, O:L landscape; spike 2026-11-07); ConfigureNotify then brings the size.
    void setOrientation(Orientation o) override {
        if (!dpy_) return;
        std::string title = title_;
        title.back() = o == Orientation::Landscape ? 'L' : 'U';
        XStoreName(dpy_, win_, title.c_str());
        XFlush(dpy_);
    }

    void present(const Canvas& canvas, Rect r, Waveform w) override {
        r = r.intersected({0, 0, w_, h_});
        if (r.empty() || !dpy_) return;
        canvas.toGray(backing_.data(), w_, r);
        putAndRefresh(r, w);
    }

    bool repaintsOnForeground() const override { return true; }  // Expose: from the backing store
    bool systemInk() const override { return wet_ && wet_->connected(); }
    bool setSystemInk(bool on) override {
        return !wet_ || wet_->setMode(on ? WetInk::Mode::Pen : WetInk::Mode::Paused);
    }

    // Waits for input, a worker's wake or `timeout_ms` (-1: no limit; nothing is due).
    bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) override {
        if (!dpy_) return false;
        if (uiWakePending()) timeout_ms = 0;  // e.g. one from before the pipe existed
        if (!XPending(dpy_)) {  // (XPending also sends what is queued for the server)
            // A negative fd is skipped by poll(): the pen may be gone.
            pollfd p[3] = {{ConnectionNumber(dpy_), POLLIN, 0}, {pen_ ? pen_->fd() : -1, POLLIN, 0}, {gWakePipe[0], POLLIN, 0}};
            poll(p, 3, timeout_ms);
            if (p[1].fd >= 0 && (p[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                log(LogLevel::Warn, "pen: the digitizer went away");
                pen_.reset();
                InputEvent e;  // whatever it was doing has ended
                e.kind = InputEvent::Kind::Pen;
                e.pen.type = PenEvent::Type::Leave;
                e.pen.t_ms = nowMs();
                out.push_back(e);
            }
        }
        // Taken before the loop reads the workers' queues: a wake after this one wakes the next wait.
        uiWakeTaken();
        if (gWakePipe[0] >= 0) {
            char buf[64];
            while (::read(gWakePipe[0], buf, sizeof buf) > 0) {
            }
        }
        // The pen first: its strokes are what must not wait (palm touches queued behind it are
        // then dropped by the app while the pen is near).
        if (pen_ && wet_ && nowMs() - lastTurnCheckMs_ > 1000) {  // a 180-degree turn sends no event
            lastTurnCheckMs_ = nowMs();
            updatePenView();
        }
        if (pen_) pen_->read(out, nowMs());
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
                case ConfigureNotify:
                    if (ev.xconfigure.width != w_ || ev.xconfigure.height != h_) resize(ev.xconfigure.width, ev.xconfigure.height, out);
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

    // The window turned (or the window manager settled its size): new backing store, FBInk state
    // for the rotated framebuffer, and a Resize for the app.
    void resize(int w, int h, std::vector<InputEvent>& out) {
        w_ = w;
        h_ = h;
        Window child;
        XTranslateCoordinates(dpy_, win_, DefaultRootWindow(dpy_), 0, 0, &originX_, &originY_, &child);
        if (image_) {
            image_->data = nullptr;
            XDestroyImage(image_);
        }
        backing_.assign((size_t)w_ * h_, 0xff);
        image_ = XCreateImage(dpy_, DefaultVisual(dpy_, DefaultScreen(dpy_)), 8, ZPixmap, 0,
                              reinterpret_cast<char*>(backing_.data()), w_, h_, 8, w_);
        if (fb_ >= 0 && fbink_reinit(fb_, &cfg_) < 0) log(LogLevel::Warn, "FBInk reinit after a turn failed");
        updatePenView();
        // The canvas as the screen is turned now, from the window (Xlib's DisplayWidth keeps the
        // size from when the display was opened and doesn't follow a turn).
        if (wet_) wet_->setCanvas(originX_ + w_, originY_ + h_);
        log(LogLevel::Info, "window %dx%d at (%d,%d)", w_, h_, originX_, originY_);
        InputEvent e;
        e.kind = InputEvent::Kind::Resize;
        out.push_back(e);
    }

    void putAndRefresh(Rect r, Waveform w) {
        XPutImage(dpy_, win_, gc_, image_, r.x, r.y, r.x, r.y, r.w, r.h);
        XSync(dpy_, False);
        if (fb_ < 0) return;
        cfg_.wfm_mode = w == Waveform::Ink    ? WFM_A2
                   : w == Waveform::Fast ? WFM_DU
                   : w == Waveform::Ui   ? WFM_GL16
                                         : WFM_GC16;
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
    std::unique_ptr<EvdevPen> pen_;
    std::string title_ = kWindowTitle;
    std::unique_ptr<WetInk> wet_;
    int penTurns_ = -1;
    int64_t lastTurnCheckMs_ = 0;
};

}  // namespace

std::unique_ptr<Display> makeDisplay() { return std::make_unique<X11Display>(); }

void displayEmergencyExit() { WetInk::emergencyOff(); }
void displayResetSystemInk() { WetInk::forceOff(); }

const char* fontDir() { return "/usr/java/lib/fonts"; }

}  // namespace zk
