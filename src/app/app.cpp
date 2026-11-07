#include "app/app.h"

#include <algorithm>
#include <chrono>

#include "core/log.h"
#include "ui/metrics.h"
#include "store/db.h"
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

App::~App() { closeAll(); }

void App::closeAll() {
    while (!graveyard_.empty()) graveyard_.pop_back();
    for (int t = 0; t < kTabs; ++t) {
        const int tab = (tab_ + t) % kTabs;  // the visible tab first
        while (!stacks_[tab].empty()) stacks_[tab].pop_back();
    }
}

void App::setRoot(int tab, std::unique_ptr<Screen> s) {
    stacks_[tab].clear();
    stacks_[tab].push_back(std::move(s));
}

Screen* App::visibleTop() const { return stacks_[tab_].empty() ? nullptr : stacks_[tab_].back().get(); }

namespace {

}  // namespace

void App::toast(const std::string& text) {
    toast_ = text;
    lastToast_ = text;
    toastUntil_ = clock_() + 3000;
    pending_.push_back({toastRect(), Waveform::Ui});
}

Rect App::toastRect() const { return {kMargin, canvas_.height() - kTabBarH - 130, canvas_.width() - 2 * kMargin, 100}; }

Rect App::tabBar() const { return {0, canvas_.height() - kTabBarH, canvas_.width(), kTabBarH}; }

void App::start() {
    showTop();
    pending_.clear();
    pending_.push_back({{0, 0, canvas_.width(), canvas_.height()}, Waveform::Full});
    flush();
    log(LogLevel::Info, "first screen presented");
}

void App::showTop() {
    Screen* s = visibleTop();
    if (!s) return;
    if (s->wantsKeyboard()) keyboard_.show();
    else keyboard_.hide();
    caller_ = s;
    guard([&] { s->onShow(*this); });
    caller_ = nullptr;
    deliverHeld(s);
    redrawAll(Waveform::Quality);
    applyOrientation();
}

void App::applyOrientation() {
    Screen* s = visibleTop();
    const bool landscape = s && s->isReading() && readingLandscape_ && readingLandscape_();
    const Orientation want = landscape ? Orientation::Landscape : Orientation::Portrait;
    if (want == orientation_) return;
    orientation_ = want;
    log(LogLevel::Info, "orientation: %s", landscape ? "landscape" : "portrait");
    display_.setOrientation(want);  // a Resize event follows
}

void App::resized() {
    if (display_.width() == canvas_.width() && display_.height() == canvas_.height()) return;
    Metrics m = metrics();
    m.width = display_.width();
    m.height = display_.height();
    m.origin = display_.origin();
    setMetrics(m);
    log(LogLevel::Info, "window now %dx%d at (%d,%d)", m.width, m.height, m.origin.x, m.origin.y);
    canvas_.resize(m.width, m.height);
    pending_.clear();
    if (Screen* s = visibleTop()) {
        caller_ = s;
        guard([&] { s->onResize(*this); });
        caller_ = nullptr;
    }
    redrawAll(Waveform::Full);
}

void App::deliverHeld(Screen* s) {
    // Forget screens that are gone (their address may be reused by a new one).
    for (auto it = held_.begin(); it != held_.end();) {
        bool alive = false;
        for (const auto& stack : stacks_)
            for (const auto& x : stack) alive = alive || x.get() == it->first;
        it = alive ? std::next(it) : held_.erase(it);
    }
    auto it = held_.find(s);
    if (it == held_.end()) return;
    std::vector<SyncEvent> events = std::move(it->second);
    held_.erase(it);
    for (const auto& e : events) {
        caller_ = s;
        guard([&] { s->onSyncEvent(e, *this); });
        caller_ = nullptr;
    }
}

void App::replaceRoot(int tab, std::unique_ptr<Screen> s) {
    for (auto& old : stacks_[tab]) graveyard_.push_back(std::move(old));
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
    graveyard_.push_back(std::move(stacks_[tab_].back()));
    stacks_[tab_].pop_back();
    showTop();
}

void App::switchTab(int index) {
    if (index < 0 || index >= kTabs || stacks_[index].empty()) return;
    if (index == tab_) {
        // Tapping the current tab goes back to its root.
        if (stacks_[tab_].size() > 1) {
            while (stacks_[tab_].size() > 1) {
                graveyard_.push_back(std::move(stacks_[tab_].back()));
                stacks_[tab_].pop_back();
            }
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

void App::redrawAll(Waveform w) {
    if (caller_ && caller_ != visibleTop()) return;  // hidden screens repaint when shown
    pending_.push_back({{0, 0, canvas_.width(), canvas_.height()}, w});
}

void App::guard(const std::function<void()>& f) {
    try {
        f();
    } catch (const DbError& e) {
        // Busy behind a long sync transaction, disk full, storage gone: say so, keep running.
        log(LogLevel::Warn, "library error on the UI thread: %s", e.what());
        toast(e.diskFull() ? "Storage is full." : "The library is busy. Try again in a moment.");
    }
}

void App::handle(const Gesture& g) {
    if (g.type == GestureType::Tap || g.type == GestureType::LongPress || g.type == GestureType::Swipe ||
        g.type == GestureType::DragEnd || g.type == GestureType::LongPressUp) {
        static const char* names[] = {"tap", "long press", "drag start", "drag move", "drag end", "swipe", "lift"};
        log(LogLevel::Info, "%s at %d,%d", names[static_cast<int>(g.type)], g.pos.x, g.pos.y);
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
            display_.present(canvas_, tabBar(), Waveform::Ui);
            switchTab(i);
        }
        return;
    }
    caller_ = s;
    guard([&] { s->onGesture(g, *this); });
    caller_ = nullptr;
}

void App::beat(const char* doing) {
    const int64_t now = clock_();
    // "idle" is the wait for input: only work can be slow.
    if (phaseStartMs_ >= 0 && now - phaseStartMs_ > 700 && std::string(phase_) != "idle") {
        if (slowLog_) slowLog_(now - phaseStartMs_, phase_);
        else log(LogLevel::Warn, "slow UI step: %lld ms in %s", static_cast<long long>(now - phaseStartMs_), phase_);
    }
    phase_ = doing;
    phaseStartMs_ = now;
    if (heartbeat_) heartbeat_(doing);
}

void App::step(int timeout_ms) {
    beat("idle");
    std::vector<InputEvent> events;
    if (!display_.pollEvents(events, timeout_ms)) {
        closed_ = true;
        return;
    }
    beat("input");
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
                    guard([&] { s->onKey(e.key, *this); });
                    caller_ = nullptr;
                }
                break;
            case InputEvent::Kind::Background:
                log(LogLevel::Info, "background");
                if (lifecycle_) lifecycle_(false);
                break;
            case InputEvent::Kind::Resize:
                resized();
                break;
            case InputEvent::Kind::Foreground:
                log(LogLevel::Info, "foreground");
                redrawAll(Waveform::Full);
                if (lifecycle_) lifecycle_(true);
                break;
        }
    }
    auto ticked = gestures_.tick(clock_());
    gestures.insert(gestures.end(), ticked.begin(), ticked.end());
    for (const Gesture& g : gestures) {
        if (quit_) break;
        handle(g);
    }

    beat("sync events");
    SyncEvent ev;
    while (sync_->poll(ev)) {
        if (hook_) {
            caller_ = nullptr;
            guard([&] { hook_(ev, *this); });
        }
        const bool reloads = ev.kind == SyncEvent::Kind::SyncFinished || ev.kind == SyncEvent::Kind::PushFinished ||
                             ev.kind == SyncEvent::Kind::AnnotationsReady;
        for (auto& stack : stacks_)
            for (auto& s : stack) {
                if (reloads && s.get() != visibleTop()) {
                    auto& held = held_[s.get()];
                    // Keep the latest of each (per book, for annotations).
                    held.erase(std::remove_if(held.begin(), held.end(),
                                              [&](const SyncEvent& h) {
                                                  return h.kind == ev.kind && h.attachmentKey == ev.attachmentKey;
                                              }),
                               held.end());
                    held.push_back(ev);
                    continue;
                }
                caller_ = s.get();
                guard([&] { s->onSyncEvent(ev, *this); });
                caller_ = nullptr;
            }
    }
    if (toastShown_ && clock_() >= toastUntil_) {
        toast_.clear();
        toastShown_ = false;
        pending_.push_back({toastRect(), Waveform::Quality});
    }
    beat("screen tick");
    if (Screen* s = visibleTop()) {
        caller_ = s;
        guard([&] { s->onTick(*this); });
        caller_ = nullptr;
    }
    if (watchdog_ && clock_() - lastWatchdogMs_ >= watchdogPeriodMs_) {
        lastWatchdogMs_ = clock_();
        if (!watchdog_()) quit_ = true;
    }
    auto deferred = std::move(deferred_);
    deferred_.clear();
    for (auto& f : deferred) guard(f);
    beat("drawing");
    flush();
    while (!graveyard_.empty()) graveyard_.pop_back();  // newest (topmost) first
    beat("idle");
}

void App::flush() {
    if (pending_.empty() || !visibleTop()) return;
    const Rect all{0, 0, canvas_.width(), canvas_.height()};
    Rect fast, ui, quality, full;
    for (const auto& [r, w] : pending_) {
        Rect& u = w == Waveform::Fast ? fast : w == Waveform::Ui ? ui : w == Waveform::Quality ? quality : full;
        u = u.united(r.intersected(all));
    }
    pending_.clear();
    if (quality == all && ++fullChanges_ % kFlashEvery == 0) {
        full = all;
        quality = {};
    }
    guard([&] { visibleTop()->draw(canvas_); });
    if (visibleTop()->showsTabBar()) drawTabBar(canvas_.cr(), tabBar(), kTabNames, tab_);
    if (!toast_.empty()) {
        drawToast(canvas_.cr(), toastRect(), toast_);
        toastShown_ = true;
    }
    if (!full.empty()) {
        display_.present(canvas_, full.united(quality).united(ui).united(fast), Waveform::Full);
        return;
    }
    if (!fast.empty()) display_.present(canvas_, fast, Waveform::Fast);
    if (!ui.empty()) display_.present(canvas_, ui, Waveform::Ui);
    if (!quality.empty()) display_.present(canvas_, quality, Waveform::Quality);
}

int App::run() {
    while (!quit_ && !closed_ && visibleTop()) step(50);
    return exitCode_;
}

}  // namespace zk
