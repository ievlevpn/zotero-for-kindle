#pragma once
// A Nav for screen tests: records damages, pushes and pops; owns a FakeSync and a keyboard.

#include "fake_sync.h"
#include "platform/device_profile.h"
#include "platform/keyboard.h"
#include "ui/metrics.h"
#include "screens/screen.h"

#include <memory>
#include <string>
#include <vector>

namespace zk::test {

class FakeKeyboard : public Keyboard {
public:
    void show() override { shown = true; ++shows; }
    void hide() override { shown = false; ++hides; }
    // The device's keyboard at the bottom of the window (the PW5's: {0, 980, 1236, 567}).
    Rect bounds() const override {
        if (!shown) return {};
        if (!fixed.empty()) return fixed;  // what lipc reported, when a test says so
        const int h = profileFor(metrics().device, 300).keyboardHeightPx;
        return {0, metrics().height - h, metrics().width, h};
    }
    bool shown = false;
    int shows = 0, hides = 0;
    Rect fixed;
};

// Keeps the plugin events the screens send.
class RecordingSink : public PluginSink {
public:
    void emit(PluginEvent e) override { events.push_back(std::move(e)); }
    bool listening() const override { return listens; }
    bool listens = true;  // false: as when no plugin is on (nothing should be built or emitted)
    std::vector<PluginEvent> events;
    // The events of one name, in order.
    std::vector<PluginEvent> named(const std::string& name) const {
        std::vector<PluginEvent> out;
        for (const auto& e : events)
            if (e.name == name) out.push_back(e);
        return out;
    }
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
    void restart() override { restarted = true; }
    void toast(const std::string& text) override { toasts.push_back(text); }
    const Screen* top() const override { return visible; }
    void orientationChanged() override { ++orientationChanges; }
    void paintNow(Rect r, Waveform w, const std::function<void(cairo_t*)>&) override { painted.push_back({r, w}); }
    bool systemInk() const override { return hasSystemInk; }
    void setSystemInk(bool on) override { systemInkOn = on; }
    PluginSink& plugins() override { return pluginSink; }
    PluginEntries* pluginEntries() override { return entries; }
    RecordingSink pluginSink;
    PluginEntries* entries = nullptr;
    bool hasSystemInk = false, systemInkOn = false;
    int orientationChanges = 0;

    struct Damage {
        Rect rect;
        Waveform waveform;
    };
    std::vector<Damage> damages;
    std::vector<Damage> painted;  // paintNow: drawn and shown at once
    std::vector<std::unique_ptr<Screen>> pushed;
    std::vector<std::string> toasts;
    const Screen* visible = nullptr;
    FakeSync syncFake;
    FakeKeyboard keyboardFake;
    int pops = 0, tab = -1;
    int64_t now = 1'000'000;
    bool quitted = false;
    bool restarted = false;

private:
    Library& lib_;
};

inline Gesture tapAt(Point p) { return Gesture{GestureType::Tap, p, p}; }
inline Gesture swipe(Direction d) { return Gesture{GestureType::Swipe, {600, 700}, {600, 700}, d}; }

}  // namespace zk::test
