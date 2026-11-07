#pragma once
// A Nav for screen tests: records damages, pushes and pops; owns a FakeSync and a keyboard.

#include "fake_sync.h"
#include "platform/keyboard.h"
#include "screens/screen.h"

#include <memory>
#include <vector>

namespace zk::test {

class FakeKeyboard : public Keyboard {
public:
    void show() override { shown = true; ++shows; }
    void hide() override { shown = false; ++hides; }
    Rect bounds() const override { return shown ? Rect{0, 980, 1236, 567} : Rect{}; }
    bool shown = false;
    int shows = 0, hides = 0;
};

class FakeNav : public Nav {
public:
    explicit FakeNav(Library& lib) : lib_(lib) {}
    void push(std::unique_ptr<Screen> s) override { pushed.push_back(std::move(s)); }
    void pop() override { ++pops; }
    void switchTab(int i) override { tab = i; }
    void damage(Rect r, Waveform w) override { damages.push_back({r, w}); }
    void redrawAll(Waveform w) override { damages.push_back({{0, 0, 1236, 1547}, w}); }
    SyncControl& sync() override { return syncFake; }
    Library& library() override { return lib_; }
    Keyboard& keyboard() override { return keyboardFake; }
    int64_t nowMs() const override { return now; }
    void quit() override { quitted = true; }

    struct Damage {
        Rect rect;
        Waveform waveform;
    };
    std::vector<Damage> damages;
    std::vector<std::unique_ptr<Screen>> pushed;
    FakeSync syncFake;
    FakeKeyboard keyboardFake;
    int pops = 0, tab = -1;
    int64_t now = 1'000'000;
    bool quitted = false;

private:
    Library& lib_;
};

inline Gesture tapAt(Point p) { return Gesture{GestureType::Tap, p, p}; }
inline Gesture swipe(Direction d) { return Gesture{GestureType::Swipe, {600, 700}, {600, 700}, d}; }

}  // namespace zk::test
