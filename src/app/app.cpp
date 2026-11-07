#include "app/app.h"

#include <chrono>

#include "core/log.h"
#include "ui/widgets.h"

namespace zk {

namespace {

int64_t steadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

const std::vector<std::string> kTabNames = {"Home", "Library", "Collections", "Notes"};

}  // namespace

App::App(Display& display, Library& lib, SyncControl& sync, Keyboard& keyboard, std::function<int64_t()> clock)
    : display_(display), lib_(lib), sync_(&sync), keyboard_(keyboard), clock_(clock ? std::move(clock) : steadyMs),
      canvas_(display.width(), display.height()) {}

void App::setRoot(int tab, std::unique_ptr<Screen> s) {
    stacks_[tab].clear();
    stacks_[tab].push_back(std::move(s));
}

Screen* App::visibleTop() const { return stacks_[tab_].empty() ? nullptr : stacks_[tab_].back().get(); }

namespace {
Rect toastRect() { return {kMargin, 1547 - kTabBarH - 130, 1236 - 2 * kMargin, 100}; }
}  // namespace

void App::toast(const std::string& text) {
    toast_ = text;
    toastUntil_ = clock_() + 3000;
    pending_.push_back({toastRect(), Waveform::Fast});
}

Rect App::tabBar() const { return {0, canvas_.height() - kTabBarH, canvas_.width(), kTabBarH}; }

void App::start() {
    showTop();
    pending_.clear();
    pending_.push_back({{0, 0, canvas_.width(), canvas_.height()}, Waveform::Full});
    flush();
}

void App::showTop() {
    Screen* s = visibleTop();
    if (!s) return;
    if (s->wantsKeyboard()) keyboard_.show();
    else keyboard_.hide();
    caller_ = s;
    s->onShow(*this);
    caller_ = nullptr;
    redrawAll(Waveform::Quality);
}

void App::replaceRoot(int tab, std::unique_ptr<Screen> s) {
    stacks_[tab].clear();
    stacks_[tab].push_back(std::move(s));
    tab_ = tab;
    showTop();
}

void App::push(std::unique_ptr<Screen> s) {
    stacks_[tab_].push_back(std::move(s));
    showTop();
}

void App::pop() {
    if (stacks_[tab_].size() <= 1) return;
    stacks_[tab_].pop_back();
    showTop();
}

void App::switchTab(int index) {
    if (index < 0 || index >= kTabs || stacks_[index].empty()) return;
    if (index == tab_) {
        // Tapping the current tab goes back to its root.
        if (stacks_[tab_].size() > 1) {
            stacks_[tab_].resize(1);
            showTop();
        }
        return;
    }
    tab_ = index;
    showTop();
}

void App::damage(Rect r, Waveform w) {
    if (caller_ && caller_ != visibleTop()) return;  // a hidden screen updated its state; it redraws when shown
    pending_.push_back({r, w});
}

void App::redrawAll(Waveform w) { pending_.push_back({{0, 0, canvas_.width(), canvas_.height()}, w}); }

void App::handle(const Gesture& g) {
    if (g.type == GestureType::LongPress && g.pos.y < kHeaderH) {
        log(LogLevel::Info, "long press on header: exit");
        quit_ = true;
        return;
    }
    Screen* s = visibleTop();
    if (!s) return;
    if (s->showsTabBar() && tabBar().contains(g.pos)) {
        if (g.type != GestureType::Tap) return;
        for (int i = 0; i < kTabs; ++i) {
            Rect r = tabRect(tabBar(), i, kTabs);
            if (!r.contains(g.pos)) continue;
            // Instant feedback on the tab bar (fast waveform), then the new screen.
            drawTabBar(canvas_.cr(), tabBar(), kTabNames, i);
            display_.present(canvas_, tabBar(), Waveform::Fast);
            switchTab(i);
        }
        return;
    }
    caller_ = s;
    s->onGesture(g, *this);
    caller_ = nullptr;
}

void App::step(int timeout_ms) {
    std::vector<InputEvent> events;
    if (!display_.pollEvents(events, timeout_ms)) {
        closed_ = true;
        return;
    }
    std::vector<Gesture> gestures;
    for (const InputEvent& e : events) {
        switch (e.kind) {
            case InputEvent::Kind::Touch: {
                auto g = gestures_.feed(e.touch);
                gestures.insert(gestures.end(), g.begin(), g.end());
                break;
            }
            case InputEvent::Kind::Key:
                if (Screen* s = visibleTop()) {
                    caller_ = s;
                    s->onKey(e.key, *this);
                    caller_ = nullptr;
                }
                break;
            case InputEvent::Kind::Background: log(LogLevel::Info, "background"); break;
            case InputEvent::Kind::Foreground:
                log(LogLevel::Info, "foreground");
                redrawAll(Waveform::Full);
                break;
        }
    }
    auto ticked = gestures_.tick(clock_());
    gestures.insert(gestures.end(), ticked.begin(), ticked.end());
    for (const Gesture& g : gestures) {
        if (quit_) break;
        handle(g);
    }

    SyncEvent ev;
    while (sync_->poll(ev)) {
        if (hook_) {
            caller_ = nullptr;
            hook_(ev, *this);
        }
        for (auto& stack : stacks_)
            for (auto& s : stack) {
                caller_ = s.get();
                s->onSyncEvent(ev, *this);
                caller_ = nullptr;
            }
    }
    if (toastShown_ && clock_() >= toastUntil_) {
        toast_.clear();
        toastShown_ = false;
        pending_.push_back({toastRect(), Waveform::Quality});
    }
    if (Screen* s = visibleTop()) {
        caller_ = s;
        s->onTick(*this);
        caller_ = nullptr;
    }
    auto deferred = std::move(deferred_);
    deferred_.clear();
    for (auto& f : deferred) f();
    flush();
}

void App::flush() {
    if (pending_.empty() || !visibleTop()) return;
    const Rect all{0, 0, canvas_.width(), canvas_.height()};
    Rect fast, quality, full;
    for (const auto& [r, w] : pending_) {
        Rect& u = w == Waveform::Fast ? fast : w == Waveform::Quality ? quality : full;
        u = u.united(r.intersected(all));
    }
    pending_.clear();
    if (quality == all && ++fullChanges_ % kFlashEvery == 0) {
        full = all;
        quality = {};
    }
    visibleTop()->draw(canvas_);
    if (visibleTop()->showsTabBar()) drawTabBar(canvas_.cr(), tabBar(), kTabNames, tab_);
    if (!toast_.empty()) {
        drawToast(canvas_.cr(), toastRect(), toast_);
        toastShown_ = true;
    }
    if (!full.empty()) {
        display_.present(canvas_, full.united(quality).united(fast), Waveform::Full);
        return;
    }
    if (!fast.empty()) display_.present(canvas_, fast, Waveform::Fast);
    if (!quality.empty()) display_.present(canvas_, quality, Waveform::Quality);
}

int App::run() {
    while (!quit_ && !closed_) step(50);
    return 0;
}

}  // namespace zk
