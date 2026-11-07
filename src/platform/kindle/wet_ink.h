#pragma once
// The Scribe's "wet ink": the firmware's WetInkHandler draws pen strokes straight into the
// framebuffer over one registered X window, with no app in the loop, then hands the finished
// stroke to the app (which draws it as "dry" ink). We use it only for the instant line: the app
// still reads the pen itself and saves strokes as Zotero ink annotations; the strokes the handler
// offers are acknowledged and dropped. Protocol: docs/scribe-pen.md (SysV IPC, observed).

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace zk {

class WetInk {
public:
    // nullptr when the device has no wet ink handler (anything but a Scribe).
    static std::unique_ptr<WetInk> open();
    ~WetInk();

    // Connects and registers `window` (must be the top window: full screen, no status bar).
    bool connect(unsigned long window, int canvasW, int canvasH);
    // Pen: the handler draws the tip's line; Paused: it draws nothing (menus, other screens).
    enum class Mode { Pen, Paused };
    void setMode(Mode);
    bool connected() const { return connected_; }
    void disconnect();  // also on destruction; leaves the handler off
    // For crash paths (signal handlers, the stall watchdog): turns the handler off and ends the
    // session with async-signal-safe calls only. No-op when nothing is connected.
    static void emergencyOff();

private:
    WetInk(int sem, int queue) : sem_(sem), queue_(queue) {}
    bool post(int num, int delta);
    bool take(int num, int timeoutMs);  // 0: don't wait
    bool send(long type, const void* data, size_t n);
    bool sendMode(uint8_t m);
    bool handshake();
    void ackLoop();

    int sem_, queue_;
    std::atomic<bool> connected_{false}, stop_{false};
    std::thread acker_;
    Mode mode_ = Mode::Paused;
};

}  // namespace zk
