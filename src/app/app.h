#pragma once
// The UI thread's loop: input -> gestures -> the visible screen; sync events -> all screens;
// damaged regions -> one present per waveform (spec §7.6).

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include "screens/screen.h"

namespace zk {

class App final : public Nav {
public:
    static constexpr int kTabs = 4;
    static constexpr int kFlashEvery = 6;  // every 6th full-screen change is a Full refresh

    App(Display&, Library&, SyncControl&, Keyboard&, std::function<int64_t()> clock = {});

    void setRoot(int tab, std::unique_ptr<Screen>);
    void start();               // shows tab 0 with a Full refresh
    void step(int timeout_ms);  // one loop iteration
    int run();                  // until quit or the window closes; returns the exit code
    bool quitting() const { return quit_; }

    // Nav
    void push(std::unique_ptr<Screen>) override;
    void pop() override;
    void switchTab(int index) override;
    void damage(Rect, Waveform) override;
    void redrawAll(Waveform) override;
    SyncControl& sync() override { return sync_; }
    Library& library() override { return lib_; }
    Keyboard& keyboard() override { return keyboard_; }
    int64_t nowMs() const override { return clock_(); }
    void quit() override { quit_ = true; }
    void toast(const std::string& text) override;
    const Screen* top() const override { return visibleTop(); }

    // Called with every sync event before the screens see it (the open flow's download states).
    void setEventHook(std::function<void(const SyncEvent&, Nav&)> hook) { hook_ = std::move(hook); }

private:
    Screen* visibleTop() const;
    void showTop();
    void handle(const Gesture&);
    void flush();
    Rect tabBar() const;

    Display& display_;
    Library& lib_;
    SyncControl& sync_;
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
    std::function<void(const SyncEvent&, Nav&)> hook_;
    std::string toast_;
    int64_t toastUntil_ = 0;
    bool toastShown_ = false;
    bool closed_ = false;
};

}  // namespace zk
