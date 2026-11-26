// Kindle display: an X11 window under the awesome WM + explicit e-ink refresh via FBInk.
// See research/spikes/s2-s5-window-refresh.md.
// Color (Colorsoft, platform/color.h): the X path when X has a 24-bit TrueColor visual (a 32-bit
// image), else the framebuffer path (X keeps its 8-bit gray; a colored region's RGB goes to the
// framebuffer through FBInk right before the refresh, gray ones are X's alone). Color off is the
// gray path as before.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <fbink.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include <bit>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/wake.h"
#include "platform/color.h"
#include "platform/device_profile.h"
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

// SIGWINCH, from the launcher when Zotero is started again while it runs (assets/launcher/Zotero.sh):
// come to the front. The handler only notes it and wakes the loop (both async-signal-safe).
volatile sig_atomic_t gRaiseRequested = 0;

void onRaiseSignal(int) {
    gRaiseRequested = 1;
    writeWake();
}

// MIT-SHM: the X server reads an update's pixels from shared memory instead of the socket (a
// Scribe's page is 4.6 MB, which Xlib otherwise sends in pieces for the server to copy out).
// The pixels are copied in from the app's own image for each update; off with ZK_NO_SHM=1, and
// whenever the server can't attach (the plain XPutImage then, as always).
int gShmError = 0;
int onShmError(::Display*, XErrorEvent*) {
    gShmError = 1;
    return 0;
}

class ShmImage {
public:
    ~ShmImage() { release(); }
    // An image of this visual, depth and size in shared memory; false: not possible here.
    bool make(::Display* dpy, Visual* visual, int depth, int w, int h, int byteOrder) {
        release();
        if (std::getenv("ZK_NO_SHM") || !XShmQueryExtension(dpy)) return false;
        img_ = XShmCreateImage(dpy, visual, depth, ZPixmap, nullptr, &info_, w, h);
        if (!img_) return false;
        img_->byte_order = byteOrder;
        info_.shmid = shmget(IPC_PRIVATE, static_cast<size_t>(img_->bytes_per_line) * img_->height, IPC_CREAT | 0600);
        if (info_.shmid < 0) return fail("shmget");
        info_.shmaddr = img_->data = static_cast<char*>(shmat(info_.shmid, nullptr, 0));
        if (info_.shmaddr == reinterpret_cast<char*>(-1)) {
            info_.shmaddr = img_->data = nullptr;
            shmctl(info_.shmid, IPC_RMID, nullptr);
            return fail("shmat");
        }
        info_.readOnly = False;
        gShmError = 0;
        XErrorHandler old = XSetErrorHandler(onShmError);
        const bool attached = XShmAttach(dpy, &info_) && (XSync(dpy, False), !gShmError);
        XSetErrorHandler(old);
        shmctl(info_.shmid, IPC_RMID, nullptr);  // freed once both sides have let go
        if (!attached) return fail("XShmAttach");
        dpy_ = dpy;
        bpp_ = img_->bits_per_pixel / 8;
        return true;
    }
    explicit operator bool() const { return dpy_ != nullptr; }
    // The shared pixels and their row length in bytes (the app may draw into them directly).
    uint8_t* data() const { return reinterpret_cast<uint8_t*>(img_->data); }
    int stride() const { return img_->bytes_per_line; }
    // Region `r` of `src` (rows `srcStride` bytes apart, the same pixel format) to the window;
    // `src` may be data() itself (nothing is copied then).
    void put(Window win, GC gc, const uint8_t* src, int srcStride, Rect r) {
        if (src != data())
            for (int y = r.y; y < r.y + r.h; ++y)
                std::memcpy(img_->data + static_cast<size_t>(y) * img_->bytes_per_line + r.x * bpp_,
                            src + static_cast<size_t>(y) * srcStride + r.x * bpp_, static_cast<size_t>(r.w) * bpp_);
        XShmPutImage(dpy_, win, gc, img_, r.x, r.y, r.x, r.y, r.w, r.h, False);
    }
    void release() {
        if (dpy_) XShmDetach(dpy_, &info_);
        if (info_.shmaddr) shmdt(info_.shmaddr);
        if (img_) {
            img_->data = nullptr;
            XDestroyImage(img_);
        }
        img_ = nullptr;
        info_ = {};
        dpy_ = nullptr;
    }

private:
    bool fail(const char* what) {
        log(LogLevel::Info, "X: no shared memory (%s failed): images through the socket", what);
        release();
        return false;
    }
    ::Display* dpy_ = nullptr;  // set once attached
    XImage* img_ = nullptr;
    XShmSegmentInfo info_{};
    int bpp_ = 1;
};

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
            panelFound_ = state.has_color_panel;
            reagl_ = pageReagl(state.is_mtk, std::getenv("ZK_REAGL"));
            log(LogLevel::Info,
                "FBInk: %s (%s, id %#x), view %ux%u, %u bpp, pixel format %u, rotation %u, color panel %s, MTK %s",
                state.device_name, state.device_codename, static_cast<unsigned>(state.device_id), state.view_width,
                state.view_height, state.bpp, static_cast<unsigned>(state.pixel_format), static_cast<unsigned>(state.current_rota),
                state.has_color_panel ? "yes" : "no", state.is_mtk ? "yes" : "no");
            log(LogLevel::Info, "page content: %s", reagl_ ? "REAGL" : "GC16");
        }
        rawCfg_ = cfg_;  // writing pixels (the framebuffer path): no refresh of its own, RGB without alpha
        rawCfg_.no_refresh = true;
        rawCfg_.ignore_alpha = true;
        wfmAuto_ = envIs("ZK_COLOR_WFM", "auto");
        int scr = DefaultScreen(dpy_);
        readVisual(scr);
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
        decideColor();
        makeImage();

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
        struct sigaction sa {};
        sa.sa_handler = onRaiseSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGWINCH, &sa, nullptr);
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
        shm_.release();  // while the display is still open
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
    ColorCaps colorCaps() const override { return caps_; }
    void setColorScreen(bool on) override {
        if (on == colorSetting_) return;
        waitIfColoredBefore();  // the redraw that follows mustn't land on a color refresh under way
        colorSetting_ = on;
        decideColor();
        if (path_ == ColorPath::Fb && rgb_.size() != (size_t)w_ * h_) rgb_.assign((size_t)w_ * h_, 0xffffff);
    }
    void setKeyboardShown(bool up) override {
        if (gate(gate_.setKeyboardUp(up), up ? "the system keyboard is up" : "the system keyboard went")) recolor();
    }
    std::vector<ColorPath> colorPaths() const override {
        std::vector<ColorPath> out;
        if (x32_) out.push_back(ColorPath::X);
        if (fbWritable()) out.push_back(ColorPath::Fb);
        return out;
    }
    // The device test: the gray path as color off shows it, or a color path, with this waveform.
    bool presentWith(const Canvas& canvas, Rect r, ColorPath path, ColorWfm wfm) override {
        r = r.intersected({0, 0, w_, h_});
        if (r.empty() || !dpy_) return false;
        if ((path == ColorPath::X && !x32_) || (path == ColorPath::Fb && !fbWritable())) return false;
        if (path == ColorPath::Fb && !gate_.allowed()) {
            log(LogLevel::Warn, "color test: the window is covered or behind: no framebuffer writes");
            return false;
        }
        const ColorPath saved = path_;
        path_ = path;
        if (path == ColorPath::Fb && rgb_.size() != (size_t)w_ * h_) rgb_.assign((size_t)w_ * h_, 0xffffff);
        waitIfColoredBefore();
        const bool colored = convert(canvas, r);
        log(LogLevel::Info, "color test: path %s, %s, region %scolored", colorPathName(path), colorWfmName(wfm),
            colored ? "" : "not ");
        put(r);
        // Without a flash (the test flashed the screen white before), as the app's own updates.
        const bool shown = fb_ >= 0 && (path != ColorPath::Fb || !fbWritesRegion(colored, true) || writeFb(r)) &&
                           refresh(r, Waveform::Quality, wfm, colored);
        if (fb_ >= 0) {
            const auto t0 = std::chrono::steady_clock::now();
            const int rc = fbink_wait_for_complete(fb_, LAST_MARKER);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
            log(LogLevel::Info, "color test: refresh done in %lld ms (wait %d)", static_cast<long long>(ms.count()), rc);
        }
        path_ = saved;
        if (!fbRawOk_) decideColor();  // a failed write ended the framebuffer path
        else if (path_ != ColorPath::Fb) releaseFbBuffers();
        return shown;
    }
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
        const int64_t t0 = nowMs();
        if (path_ == ColorPath::Gray && !x32_) {  // color off on an 8-bit X: as it always was
            canvas.toGray(pix8_, w_, r);
            putAndRefresh(r, w);
        } else {
            waitIfColoredBefore();
            putAndRefresh(r, w, convert(canvas, r));
        }
        if (static_cast<int64_t>(r.w) * r.h * 2 >= static_cast<int64_t>(w_) * h_)  // a page, not a button
            log(LogLevel::Debug, "presented %dx%d (%s) in %lld ms", r.w, r.h,
                w == Waveform::Full ? "full" : w == Waveform::Quality ? (reagl_ ? "REAGL" : "GC16") : "other",
                static_cast<long long>(nowMs() - t0));
    }

    bool repaintsOnForeground() const override { return true; }  // Expose: from the backing store
    bool systemInk() const override { return wet_ && wet_->connected(); }
    // The handler's 3.5 (its notebook's default pen) draws about the app's 7 px line.
    void setSystemInkWidth(int px) override {
        if (wet_) wet_->setThickness(px / 2.0f);
    }
    bool setSystemInk(bool on) override {
        return !wet_ || wet_->setMode(on ? WetInk::Mode::Pen : WetInk::Mode::Paused);
    }

    // Waits for input, a worker's wake or `timeout_ms` (-1: no limit; nothing is due).
    bool pollEvents(std::vector<InputEvent>& out, int timeout_ms) override {
        if (!dpy_) return false;
        if (uiWakePending()) timeout_ms = 0;  // e.g. one from before the pipe existed
        if (!XPending(dpy_)) {  // (XPending also sends what is queued for the server)
            // A negative fd is skipped by poll(): the pen may be gone. In the background the app
            // ignores the pen, so a pen hovering over the screensaver or a system dialog mustn't
            // wake it at the digitizer's rate; what it sent is read (and dropped) at the next wake,
            // and an overflow there is a SYN_DROPPED the decoder already handles.
            const int penFd = pen_ && !background_ ? pen_->fd() : -1;
            pollfd p[3] = {{ConnectionNumber(dpy_), POLLIN, 0}, {penFd, POLLIN, 0}, {gWakePipe[0], POLLIN, 0}};
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
        if (gRaiseRequested) {
            gRaiseRequested = 0;
            raise();
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
                    recolorDue_ |= gate(gate_.setUnobscured(ev.xvisibility.state == VisibilityUnobscured),
                                        ev.xvisibility.state == VisibilityUnobscured ? "unobscured" : "covered");
                    break;
                case Expose:
                    // Repaint the exposed area from the backing store: flash only after wake.
                    exposed_ = exposed_.united({ev.xexpose.x, ev.xexpose.y, ev.xexpose.width, ev.xexpose.height});
                    if (ev.xexpose.count == 0) {
                        const Rect shown = exposed_.intersected({0, 0, w_, h_});
                        const Waveform w = expose_.onExpose();
                        if (path_ == ColorPath::Gray) {
                            putAndRefresh(shown, w);
                        } else if (!shown.empty()) {
                            waitIfColoredBefore();
                            putAndRefresh(shown, w, coloredNow(shown));
                            if (shown == Rect{0, 0, w_, h_} && gate_.allowed()) recolorDue_ = false;  // done just now
                        }
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
        if (recolorDue_) {
            recolorDue_ = false;
            recolor();
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

    // To the front: the window manager (lab126 awesome) makes a newly mapped application window the
    // active one, so the window goes and comes back. The Expose that follows repaints it from the
    // backing store and turns the app to the foreground (seen on a PW5, 5.18.4).
    void raise() {
        log(LogLevel::Info, "asked to come to the front");
        XUnmapWindow(dpy_, win_);
        XSync(dpy_, False);
        XMapRaised(dpy_, win_);
        XSync(dpy_, False);
    }

    void setBackground(bool background, std::vector<InputEvent>& out) {
        if (background == background_) return;
        background_ = background;
        recolorDue_ |= gate(gate_.setForeground(!background), background ? "in the background" : "in front");
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
                if (ev.type == VisibilityNotify) gate_.setUnobscured(ev.xvisibility.state == VisibilityUnobscured);
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
        if (fb_ >= 0 && fbink_reinit(fb_, &cfg_) < 0) log(LogLevel::Warn, "FBInk reinit after a turn failed");
        decideColor();  // the framebuffer may not turn with the window
        makeImage();
        updatePenView();
        // The canvas as the screen is turned now, from the window (Xlib's DisplayWidth keeps the
        // size from when the display was opened and doesn't follow a turn).
        if (wet_) wet_->setCanvas(originX_ + w_, originY_ + h_);
        log(LogLevel::Info, "window %dx%d at (%d,%d)", w_, h_, originX_, originY_);
        InputEvent e;
        e.kind = InputEvent::Kind::Resize;
        out.push_back(e);
    }

    // The gray path (color off): the image to X, then the refresh with the waveform's mode.
    void putAndRefresh(Rect r, Waveform w) {
        putImage(r);
        XSync(dpy_, False);
        if (fb_ < 0) return;
        if (w == Waveform::Quality && reagl_ && refreshReagl(r)) return;
        cfg_.wfm_mode = w == Waveform::Ink    ? WFM_A2
                   : w == Waveform::Fast ? WFM_DU
                   : w == Waveform::Ui   ? WFM_GL16
                                         : WFM_GC16;
        cfg_.is_flashing = w == Waveform::Full;
        fbink_refresh(fb_, r.y + originY_, r.x + originX_, r.w, r.h, &cfg_);
    }

    // Page content as the Kindle's reader shows it: REAGL, which the driver takes only as a FULL
    // update (FBInk's is_flashing); the waveform itself doesn't flash. If the driver refuses it,
    // GC16 from then on.
    bool refreshReagl(Rect r) {
        cfg_.wfm_mode = WFM_REAGL;
        cfg_.is_flashing = true;
        const int rc = fbink_refresh(fb_, r.y + originY_, r.x + originX_, r.w, r.h, &cfg_);
        cfg_.is_flashing = false;
        if (rc >= 0) return true;
        log(LogLevel::Warn, "REAGL refresh refused (%d): page content with GC16 from now on", rc);
        reagl_ = false;
        return false;
    }

    // A color path (or gray on a 32-bit X): the image to X, the RGB to the framebuffer on that
    // path, then the refresh with the color panel's waveform for `w`.
    void putAndRefresh(Rect r, Waveform w, bool colored) {
        put(r);
        if (fb_ < 0) return;
        // Only a colored region goes to the framebuffer, and only while nothing can be over the
        // window (X clips its gray to what's visible; the framebuffer wouldn't). The rest is X's
        // gray alone, as on a gray Kindle.
        if (path_ == ColorPath::Fb && (!fbWritesRegion(colored, gate_.allowed()) || !writeFb(r))) colored = false;
        ColorWfm cw = path_ == ColorPath::Gray ? ColorWfm::Keep : colorWaveform(w, colored);
        if (cw != ColorWfm::Keep && wfmAuto_) cw = ColorWfm::Auto;
        // A gray region's page content: REAGL as on a gray Kindle (colored ones keep GCC16).
        if (w == Waveform::Quality && reagl_ && (cw == ColorWfm::Keep || cw == ColorWfm::Gc16) && refreshReagl(r)) return;
        refresh(r, w, cw, colored);
    }

    void put(Rect r) {
        putImage(r);
        XSync(dpy_, False);  // X has drawn it (into the framebuffer) before we write or refresh
    }

    // The image's region `r` to the window: through shared memory when there is some.
    void putImage(Rect r) {
        if (!shm_) {
            XPutImage(dpy_, win_, gc_, image_, r.x, r.y, r.x, r.y, r.w, r.h);
            return;
        }
        if (x32_) shm_.put(win_, gc_, reinterpret_cast<const uint8_t*>(pix32_), w_ * 4, r);
        else shm_.put(win_, gc_, pix8_, w_, r);
    }

    bool refresh(Rect r, Waveform w, ColorWfm cw, bool colored) {
        switch (cw) {
            case ColorWfm::Gl16: cfg_.wfm_mode = WFM_GL16; break;
            case ColorWfm::Glrc16: cfg_.wfm_mode = WFM_GLRC16; break;
            case ColorWfm::Gc16: cfg_.wfm_mode = WFM_GC16; break;
            case ColorWfm::Gcc16: cfg_.wfm_mode = WFM_GCC16; break;
            case ColorWfm::Auto: cfg_.wfm_mode = WFM_AUTO; break;
            case ColorWfm::Keep:
                cfg_.wfm_mode = w == Waveform::Ink ? WFM_A2 : w == Waveform::Fast ? WFM_DU : w == Waveform::Ui ? WFM_GL16 : WFM_GC16;
                break;
        }
        cfg_.is_flashing = w == Waveform::Full;
        const int rc = fbink_refresh(fb_, r.y + originY_, r.x + originX_, r.w, r.h, &cfg_);
        log(rc < 0 ? LogLevel::Warn : LogLevel::Debug, "refresh %d,%d %dx%d path %s %s%s%s: %d", r.x, r.y, r.w, r.h,
            colorPathName(path_), colorWfmName(cw), cfg_.is_flashing ? " flashing" : "", colored ? " colored" : "", rc);
        // The driver works asynchronously: the next update waits until a colored one has finished
        // (KOReader does the same on these panels), so nothing lands over it half-way.
        if (colored && cw != ColorWfm::Keep) waitBeforeNext_ = true;
        return rc >= 0;
    }

    // Logs a change of whether the framebuffer path may write (on that path); passes `reopened` on.
    bool gate(bool reopened, const char* why) {
        if (path_ == ColorPath::Fb && gate_.allowed() != fbGateLogged_) {
            fbGateLogged_ = gate_.allowed();
            log(LogLevel::Info, "color: framebuffer writes %s (%s)", fbGateLogged_ ? "resumed" : "held", why);
        }
        return reopened;
    }

    // The framebuffer path may write again: the whole window once in color from rgb_ (what was
    // shown meanwhile, under the keyboard or a dialog, went out gray). A gray window: X has it.
    void recolor() {
        if (path_ != ColorPath::Fb || !gate_.allowed() || fb_ < 0 || rgb_.size() != (size_t)w_ * h_) return;
        const Rect all{0, 0, w_, h_};
        const bool colored = isColored(rgb_.data(), w_ * 4, all);
        if (!fbWritesRegion(colored, true)) return;
        waitIfColoredBefore();
        if (!writeFb(all)) return;
        ColorWfm cw = ColorWfm::Gcc16;
        if (wfmAuto_) cw = ColorWfm::Auto;
        log(LogLevel::Info, "color: the window again in color (%s)", colorWfmName(cw));
        refresh(all, Waveform::Quality, cw, colored);
    }

    void waitIfColoredBefore() {
        if (!waitBeforeNext_ || fb_ < 0) return;
        waitBeforeNext_ = false;
        const int rc = fbink_wait_for_complete(fb_, LAST_MARKER);
        if (rc < 0) log(LogLevel::Debug, "waiting for the colored refresh: %d", rc);
    }

    // The region of the canvas into this path's buffers; whether it is colored.
    bool convert(const Canvas& canvas, Rect r) {
        auto* image32 = reinterpret_cast<uint8_t*>(pix32_);
        switch (path_) {
            case ColorPath::X: return toXrgb(canvas, image32, w_ * 4, r, rmask_, gmask_, bmask_);
            case ColorPath::Fb:
                if (x32_) toXgray(canvas, image32, w_ * 4, r, rmask_, gmask_, bmask_);
                else canvas.toGray(pix8_, w_, r);
                return toXrgb(canvas, reinterpret_cast<uint8_t*>(rgb_.data()), w_ * 4, r, 0xff0000, 0x00ff00, 0x0000ff);
            case ColorPath::Gray: break;
        }
        if (x32_) toXgray(canvas, image32, w_ * 4, r, rmask_, gmask_, bmask_);
        else canvas.toGray(pix8_, w_, r);
        return false;
    }

    // Whether what is already in this path's buffers is colored (an Expose repaints from them).
    // The X image's channels are whole bytes (readVisual), and chroma doesn't care which is which.
    bool coloredNow(Rect r) const {
        if (path_ == ColorPath::X) return isColored(pix32_, w_ * 4, r);
        if (path_ == ColorPath::Fb) return isColored(rgb_.data(), w_ * 4, r);
        return false;
    }

    // The framebuffer path: the region's RGB over the gray X just drew, without a refresh.
    bool writeFb(Rect r) {
        packed_.resize((size_t)r.w * r.h * 3);
        packRgb(rgb_.data(), w_ * 4, r, packed_.data());
        // FBInk adds its view's origin (row 0: minus its row offset) to the position: taken off here.
        const int x = r.x + originX_ - fbView_.horiOrigin, y = r.y + originY_ - (fbView_.vertOrigin - fbView_.vertOffset);
        const int rc = fbink_print_raw_data(fb_, packed_.data(), r.w, r.h, packed_.size(), static_cast<short>(x),
                                            static_cast<short>(y), &rawCfg_);
        if (rc < 0) {
            // Once, and for the session: the path is chosen again without the framebuffer (X's
            // gray, which X has drawn already, or the X path), so no more writes or warnings.
            if (rc == -ENOSYS) log(LogLevel::Warn, "color: FBInk was built without image support: no framebuffer path");
            else
                log(LogLevel::Warn, "color: writing %dx%d at (%d,%d) to the framebuffer failed (%d): no framebuffer path", r.w,
                    r.h, x, y, rc);
            fbRawOk_ = false;
            decideColor();
        }
        return rc >= 0;
    }

    // Off the framebuffer path, its buffers go (up to 4 + 3 bytes a pixel of the window).
    void releaseFbBuffers() {
        rgb_ = {};
        packed_ = {};
    }

    // X's default visual: a 32-bit image of byte-wide channels on a 24-bit TrueColor one (the X
    // path); else the 8-bit image as always.
    void readVisual(int scr) {
        Visual* v = DefaultVisual(dpy_, scr);
        const int depth = DefaultDepth(dpy_, scr);
        rmask_ = v->red_mask;
        gmask_ = v->green_mask;
        bmask_ = v->blue_mask;
        const bool trueColor = v->c_class == TrueColor;
        // Each channel a whole byte of the low three (RGB or BGR): what coloredNow reads.
        const auto r = static_cast<uint32_t>(rmask_), g = static_cast<uint32_t>(gmask_), b = static_cast<uint32_t>(bmask_);
        const bool byteChannels = (r | g | b) == 0xffffff && usableMasks(r, g, b) && std::popcount(r) == 8 &&
                                  std::popcount(g) == 8 && std::popcount(b) == 8;
        int bpp = 0, count = 0;
        if (XPixmapFormatValues* f = XListPixmapFormats(dpy_, &count)) {
            for (int i = 0; i < count; ++i)
                if (f[i].depth == depth) bpp = f[i].bits_per_pixel;
            XFree(f);
        }
        x32_ = depth >= 24 && trueColor && byteChannels && bpp == 32;
        std::string depths;
        int n = 0;
        if (int* ds = XListDepths(dpy_, scr, &n)) {
            for (int i = 0; i < n; ++i) depths += (i ? "," : "") + std::to_string(ds[i]);
            XFree(ds);
        }
        static const char* classes[] = {"StaticGray", "GrayScale", "StaticColor", "PseudoColor", "TrueColor", "DirectColor"};
        log(LogLevel::Info, "X: default visual depth %d (%d bpp), %s, masks %06lx/%06lx/%06lx; depths %s: %s image", depth, bpp,
            v->c_class >= 0 && v->c_class < 6 ? classes[v->c_class] : "?", static_cast<unsigned long>(rmask_),
            static_cast<unsigned long>(gmask_), static_cast<unsigned long>(bmask_), depths.c_str(), x32_ ? "32-bit color" : "8-bit gray");
        if (depth != 8 && !x32_) log(LogLevel::Warn, "X: a depth-%d visual the app can't draw in color: the 8-bit image as always", depth);
    }

    void makeImage() {
        if (image_) {
            image_->data = nullptr;
            XDestroyImage(image_);
        }
        const int scr = DefaultScreen(dpy_);
        if (x32_) {
            backing32_.assign((size_t)w_ * h_, rmask_ | gmask_ | bmask_);  // white
            image_ = XCreateImage(dpy_, DefaultVisual(dpy_, scr), DefaultDepth(dpy_, scr), ZPixmap, 0,
                                  reinterpret_cast<char*>(backing32_.data()), w_, h_, 32, w_ * 4);
            if (image_) image_->byte_order = std::endian::native == std::endian::little ? LSBFirst : MSBFirst;  // our uint32s
        } else {
            backing_.assign((size_t)w_ * h_, 0xff);
            image_ = XCreateImage(dpy_, DefaultVisual(dpy_, scr), 8, ZPixmap, 0, reinterpret_cast<char*>(backing_.data()), w_, h_,
                                  8, w_);
        }
        pix8_ = backing_.data();
        pix32_ = backing32_.data();
        if (image_ && shm_.make(dpy_, DefaultVisual(dpy_, scr), image_->depth, w_, h_, image_->byte_order)) {
            // Rows as long as ours (the usual widths): the app's pixels live in the shared memory
            // itself, so an update isn't copied once more; else they're copied in for each update.
            const bool direct = shm_.stride() == w_ * (x32_ ? 4 : 1);
            if (direct) {
                if (x32_) {
                    std::memcpy(shm_.data(), backing32_.data(), backing32_.size() * 4);
                    pix32_ = reinterpret_cast<uint32_t*>(shm_.data());
                    std::vector<uint32_t>().swap(backing32_);
                } else {
                    std::memcpy(shm_.data(), backing_.data(), backing_.size());
                    pix8_ = shm_.data();
                    std::vector<uint8_t>().swap(backing_);
                }
                image_->data = reinterpret_cast<char*>(shm_.data());
            }
            if (!shmLogged_) log(LogLevel::Info, "X: images through shared memory%s", direct ? "" : " (copied in)");
            shmLogged_ = true;
        }
        packed_ = {};  // sized for the old window
        if (path_ == ColorPath::Fb) rgb_.assign((size_t)w_ * h_, 0xffffff);
        else releaseFbBuffers();
    }

    // The framebuffer path may write the window's pixels: FBInk is up, can write images, and its
    // view is turned as the window is (in landscape, if the framebuffer stays portrait, the window
    // isn't where FBInk would write: no framebuffer path then).
    bool fbWritable() const {
        if (fb_ < 0 || !fbRawOk_ || !fbView_.known) return false;
        const bool windowLandscape = originX_ + w_ > originY_ + h_;
        const bool viewLandscape = fbView_.width > fbView_.height;
        return windowLandscape == viewLandscape && originX_ + w_ <= fbView_.width && originY_ + h_ <= fbView_.height;
    }

    static bool envIs(const char* name, const char* value) {
        const char* v = std::getenv(name);
        return v && std::strcmp(v, value) == 0;
    }

    // Color caps and path from the panel, the setting, X, FBInk's view and the overrides; logged.
    void decideColor() {
        if (fb_ >= 0) {
            FBInkState state{};
            fbink_get_state(&cfg_, &state);
            fbView_ = {true, static_cast<int>(state.view_width), static_cast<int>(state.view_height), state.view_hori_origin,
                       state.view_vert_origin, state.view_vert_offset};
        }
        const char* zkColor = std::getenv("ZK_COLOR");
        const char* forced = std::getenv("ZK_COLOR_PATH");
        caps_ = decideColorCaps(panelFound_, colorSetting_, zkColor);
        path_ = chooseColorPath(caps_, x32_, fbWritable(), forced);
        if (caps_.on && path_ == ColorPath::Gray) caps_.on = false;  // no way to show it
        if (path_ != ColorPath::Fb) releaseFbBuffers();
        log(LogLevel::Info,
            "color: panel %s, setting %s, ZK_COLOR=%s ZK_COLOR_PATH=%s ZK_COLOR_WFM=%s; X %s, framebuffer %s "
            "(view %dx%d, origin %d,%d offset %d; window %dx%d at %d,%d) -> path %s, color %s",
            caps_.panel ? "yes" : "no", colorSetting_ ? "on" : "off", zkColor ? zkColor : "-", forced ? forced : "-",
            wfmAuto_ ? "auto" : "-", x32_ ? "32-bit" : "8-bit", fbWritable() ? "writable" : "not writable", fbView_.width,
            fbView_.height, fbView_.horiOrigin, fbView_.vertOrigin, fbView_.vertOffset, w_, h_, originX_, originY_,
            colorPathName(path_), caps_.on ? "on" : "off");
        if (caps_.panel && colorSetting_ && !x32_ && fb_ >= 0 && fbRawOk_ && !fbWritable())
            log(LogLevel::Warn, "color: the framebuffer's view (%dx%d) isn't turned as the window (%dx%d at %d,%d): gray here",
                fbView_.width, fbView_.height, w_, h_, originX_, originY_);
    }

    ::Display* dpy_ = nullptr;
    Window win_ = 0;
    GC gc_ = nullptr;
    XImage* image_ = nullptr;
    ShmImage shm_;  // the image's pixels for the server, when it can share memory
    bool shmLogged_ = false;
    std::vector<uint8_t> backing_;     // the 8-bit image's pixels (an 8-bit X)
    std::vector<uint32_t> backing32_;  // the 32-bit image's (a 24-bit TrueColor X)
    // The image's pixels as drawn into: the vectors above, or the shared memory (ShmImage) when
    // its rows are as long (the vectors are then emptied).
    uint8_t* pix8_ = nullptr;
    uint32_t* pix32_ = nullptr;
    std::vector<uint32_t> rgb_;        // the framebuffer path: the window's colors, 0x00RRGGBB
    std::vector<uint8_t> packed_;      // a region of rgb_ as FBInk takes it
    bool x32_ = false;
    unsigned long rmask_ = 0, gmask_ = 0, bmask_ = 0;
    bool panelFound_ = false;   // FBInk's has_color_panel
    bool colorSetting_ = true;  // Display → Color
    ColorCaps caps_;
    ColorPath path_ = ColorPath::Gray;
    bool fbRawOk_ = true;          // FBInk can write pixels (built with image support)
    bool wfmAuto_ = false;         // ZK_COLOR_WFM=auto
    bool waitBeforeNext_ = false;  // a colored refresh is on its way
    bool reagl_ = false;           // page content with REAGL (pageReagl)
    // The framebuffer path writes only while nothing can be over the window (platform/color.h).
    FbColorGate gate_;
    bool fbGateLogged_ = false;  // what was last logged about it
    bool recolorDue_ = false;    // it opened again during this poll: the window in color once
    struct FbView {
        bool known = false;
        int width = 0, height = 0, horiOrigin = 0, vertOrigin = 0, vertOffset = 0;
    } fbView_;
    FBInkConfig rawCfg_;
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
