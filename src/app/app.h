#pragma once
// The UI thread's loop: input -> gestures -> the visible screen; sync events -> all screens;
// damaged regions -> one present per waveform (spec §7.6).

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "screens/plugin_confirm.h"
#include "screens/screen.h"

namespace zk {

class App final : public Nav {
public:
    static constexpr int kTabs = 4;
    static constexpr int kFlashEvery = 6;  // every 6th full-screen change is a Full refresh

    App(Display&, Library&, SyncControl&, Keyboard&, std::function<int64_t()> clock = {});
    ~App();  // screens are destroyed top first (a pushed screen may refer to the one under it)
    void closeAll();  // the same, now: main closes the readers before waiting for their workers

    void setRoot(int tab, std::unique_ptr<Screen>);
    void start();               // shows tab 0 with a Full refresh
    void step(int timeout_ms);  // one loop iteration
    int run();                  // until quit or the window closes; returns the exit code
    bool quitting() const { return quit_; }
    const std::string& lastToast() const { return lastToast_; }
    Rect tabBarRect() const { return tabBar(); }
    Rect toastRect() const;  // above the tab bar, centred

    // Nav
    void push(std::unique_ptr<Screen>) override;
    void pop() override;
    void switchTab(int index) override;
    void damage(Rect, Waveform) override;
    void redrawAll(Waveform) override;
    void paintNow(Rect, Waveform, const std::function<void(cairo_t*)>&) override;
    bool systemInk() const override { return display_.systemInk(); }
    void setSystemInk(bool on) override;
    // Palm rejection: touches are ignored while the pen is in range and this long after it left.
    static constexpr int64_t kPalmMs = 700;
    // No pen event this long: not near any more (a Leave may have been lost). A pen held still
    // sends nothing, so this is long; on the glass, longer still.
    static constexpr int64_t kPenStaleMs = 10'000;
    static constexpr int64_t kPenDownStaleMs = 30'000;
    // Ink reaches the screen at most once per frame, as one update for everything drawn since:
    // an e-ink update per pen sample queues hundreds of them and the line lags in pieces.
    static constexpr int64_t kInkFrameMs = 25;
    SyncControl& sync() override { return *sync_; }
    void setSync(SyncControl& s) { sync_ = &s; }
    // Replace a tab's root and show it (e.g. Home after Setup).
    void replaceRoot(int tab, std::unique_ptr<Screen>);
    // Run after the current loop iteration (safe to replace the calling screen).
    void defer(std::function<void()> f) { deferred_.push_back(std::move(f)); }
    void setLifecycleHook(std::function<void(bool foreground)> f) { lifecycle_ = std::move(f); }
    // Told what each part of a step is doing (the stall watchdog's beat).
    void setHeartbeat(std::function<void(const char* doing)> f) { heartbeat_ = std::move(f); }
    // Whether reading screens turn to landscape (the user's setting; read whenever it matters).
    void setReadingLandscape(std::function<bool()> f) { readingLandscape_ = std::move(f); }
    void orientationChanged() override { applyOrientation(); }
    // A part of a step that took longer than 0.7 s (default: a warning in the log).
    void setSlowStepLog(std::function<void(int64_t ms, const char* doing)> f) { slowLog_ = std::move(f); }
    // Checked every `periodMs`; returning false quits the app.
    void setWatchdog(std::function<bool()> ok, int64_t periodMs) {
        watchdog_ = std::move(ok);
        watchdogPeriodMs_ = periodMs;
    }
    Library& library() override { return lib_; }
    Keyboard& keyboard() override { return keyboard_; }
    int64_t nowMs() const override { return clock_(); }
    void quit() override { quit_ = true; }
    void restart() override {
        exitCode_ = 75;
        quit_ = true;
    }
    void toast(const std::string& text) override;
    const Screen* top() const override { return visibleTop(); }

    // Called with every sync event before the screens see it (the open flow's download states).
    void setEventHook(std::function<void(const SyncEvent&, Nav&)> hook) { hook_ = std::move(hook); }
    // Called every loop iteration after the sync events (the plugin host's events).
    void setPollHook(std::function<void()> hook) { pollHook_ = std::move(hook); }

    // The app's own confirmation (a plugin's removal): drawn over whatever screen shows, taking
    // every gesture, key and pen stroke until one of its buttons answers it; `answered` is then
    // called once. It waits while a page turn is under way or the pen is on the glass (at most
    // kTurnWaitMs; a stroke still open then gets its lift first) or a system dialog is over the app; more questions wait their turn, each shown one step after the last went.
    // Only a finger tap answers (never the pen), and only one that went down kConfirmGuardMs or
    // more after the box was first presented (one that went down earlier was meant for what was
    // there). No system ink while it shows.
    static constexpr int64_t kTurnWaitMs = 3000;
    static constexpr int64_t kConfirmGuardMs = 500;
    void confirm(uint64_t id, PluginConfirm, std::function<void(bool yes)> answered);
    // Takes the question away unanswered (waiting or shown); nothing if it is gone already.
    void withdrawConfirm(uint64_t id);
    const PluginConfirm* confirmShown() const;
    int64_t confirmShownSince() const { return shownSince_; }  // first presented (-1: not yet)

private:
    Screen* visibleTop() const;
    void showTop();
    void handle(const Gesture&);
    void guard(const std::function<void()>&);  // runs screen code; DbError -> a toast
    void flush();
    Rect tabBar() const;

    Display& display_;
    Library& lib_;
    SyncControl* sync_;
    Keyboard& keyboard_;
    std::function<int64_t()> clock_;
    Canvas canvas_;
    GestureRecognizer gestures_;
    std::array<std::vector<std::unique_ptr<Screen>>, kTabs> stacks_;
    int tab_ = 0;
    const Screen* caller_ = nullptr;  // screen currently handling a callback, for damage()
    std::vector<std::pair<Rect, Waveform>> pending_;
    int fullChanges_ = 0;
    bool quit_ = false;
    int exitCode_ = 0;
    std::function<void(const SyncEvent&, Nav&)> hook_;
    std::vector<std::function<void()>> deferred_;
    std::function<void(const char*)> heartbeat_;
    std::function<bool()> readingLandscape_;
    Orientation orientation_ = Orientation::Portrait;
    void applyOrientation();  // the visible screen and the setting decide
    void resized();           // the display turned: new metrics, canvas, layout
    // Events that make a screen reload (a finished sync, …) wait while it is hidden: reloading
    // every tab's lists at once froze the UI for 1.3 s on the Kindle after each sync.
    std::map<const Screen*, std::vector<SyncEvent>> held_;
    void deliverHeld(Screen*);
    std::function<void(int64_t, const char*)> slowLog_;
    const char* phase_ = "idle";
    int64_t phaseStartMs_ = -1;
    void beat(const char* doing);
    // Screens removed during a step; destroyed after it, so a screen may pop itself and return.
    std::vector<std::unique_ptr<Screen>> graveyard_;
    std::function<void(bool)> lifecycle_;
    std::function<bool()> watchdog_;
    int64_t watchdogPeriodMs_ = 0, lastWatchdogMs_ = 0;
    std::string toast_, lastToast_;
    int64_t toastUntil_ = 0;
    bool toastShown_ = false;
    bool closed_ = false;
    // The pen to the visible screen; a stroke it doesn't take is fed to the gestures as a touch.
    void handlePen(const PenEvent&, std::vector<Gesture>& gestures);
    static constexpr int kPenFinger = 99;  // the touch id of a pen stroke used as a finger
    bool penAsTouch_ = false;
    Point penTouchAt_;
    int64_t penLastMs_ = 0;
    void penReset(std::vector<Gesture>& gestures);
    void close(Screen&);                     // onClose, as the caller
    void retire(std::unique_ptr<Screen>);    // onClose, then to the graveyard
    Screen* lastVisible_ = nullptr;          // for onHide
    bool palmBlocked(const TouchEvent&);
    bool penNear_ = false;
    bool penDown_ = false;  // on the glass
    // A stroke the screen took (writing, erasing) and hasn't had its lift yet; where the pen was
    // last, with which end: a question showing mid-stroke gives the screen a lift there.
    bool penOwned_ = false;
    Point penAt_;
    bool penEraser_ = false;
    int64_t penLeftMs_ = -1;
    std::vector<int> palmFingers_;  // fingers that went down while the pen was near
    void presentInk(bool force);
    bool systemInkOn_ = false;
    bool background_ = false;  // a system dialog or the screensaver is over the app
    Rect inkDirty_;
    int64_t lastInkPresentMs_ = -1'000'000;
    std::function<void()> pollHook_;
    struct Question {
        uint64_t id = 0;
        PluginConfirm view;
        std::function<void(bool)> answered;
        int64_t readyMs = -1;  // first seen waiting on a page turn
    };
    std::deque<Question> questions_;  // the front one is shown when questionShown_
    bool questionShown_ = false;
    int64_t shownSince_ = -1;  // when the box was first presented (-1: not yet)
    bool answeredThisStep_ = false;   // the rest of this step's gestures are not for the next one
    void showQuestion();              // the front question, if nothing stops it now
    bool questionTakes(const Gesture&);  // true if the shown question took the gesture
};

}  // namespace zk
