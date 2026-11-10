#include "app/app.h"

#include <algorithm>
#include <chrono>
#include <climits>

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
    lastVisible_ = nullptr;
    for (int t = 0; t < kTabs; ++t) {
        const int tab = (tab_ + t) % kTabs;  // the visible tab first
        while (!stacks_[tab].empty()) {
            close(*stacks_[tab].back());
            stacks_[tab].pop_back();
        }
    }
}

void App::close(Screen& s) {
    caller_ = &s;
    guard([&] { s.onClose(*this); });
    caller_ = nullptr;
}

void App::retire(std::unique_ptr<Screen> s) {
    close(*s);
    graveyard_.push_back(std::move(s));
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

void App::confirm(uint64_t id, PluginConfirm view, std::function<void(bool)> answered) {
    questions_.push_back({id, std::move(view), std::move(answered), -1});
}

void App::withdrawConfirm(uint64_t id) {
    for (auto it = questions_.begin(); it != questions_.end(); ++it) {
        if (it->id != id) continue;
        if (it == questions_.begin() && questionShown_) {
            pending_.push_back({it->view.rect(), Waveform::Quality});  // what was under it, without ghosts
            questionShown_ = false;
            shownSince_ = -1;
        }
        questions_.erase(it);
        return;
    }
}

const PluginConfirm* App::confirmShown() const {
    return questionShown_ && !questions_.empty() ? &questions_.front().view : nullptr;
}

void App::showQuestion() {
    if (questionShown_ || answeredThisStep_ || questions_.empty() || background_ || !visibleTop()) return;
    Question& q = questions_.front();
    const int64_t now = clock_();
    // Never over a page turn or a pen on the glass: once it has landed or lifted (or seems stuck).
    const bool penOnGlass = penDown_ && now - penLastMs_ < kPenDownStaleMs;
    if (visibleTop()->midTurn() || penOnGlass) {
        if (q.readyMs < 0) q.readyMs = now;
        if (now - q.readyMs < kTurnWaitMs) return;
    }
    // A stroke the screen is writing or erasing is still open (the wait ran out): the screen gets
    // its lift now, so it ends as the user drew it; the rest of the stroke goes nowhere.
    if (penOwned_) {
        penOwned_ = false;
        PenEvent lift;
        lift.type = PenEvent::Type::Leave;
        lift.pos = penAt_;
        lift.eraser = penEraser_;
        lift.t_ms = now;
        Screen* s = visibleTop();
        caller_ = s;
        guard([&] { s->onPen(lift, *this); });
        caller_ = nullptr;
    }
    questionShown_ = true;
    shownSince_ = -1;     // stamped by flush() once the box is on the screen
    setSystemInk(false);  // and kept off while it shows (setSystemInk)
    pending_.push_back({q.view.rect(), Waveform::Ui});
}

bool App::questionTakes(const Gesture& g) {
    if (!confirmShown()) return answeredThisStep_;  // the rest of an answering tap's step
    // Only a finger answers, with a tap that went down once the box had been on screen a moment
    // (a tap that went down earlier was meant for what was there, however late it is handled).
    if (g.type != GestureType::Tap || g.finger == kPenFinger) return true;
    if (shownSince_ < 0 || g.downMs < shownSince_ + kConfirmGuardMs) return true;
    const std::optional<bool> yes = questions_.front().view.onTap(g.pos);
    if (!yes) return true;
    Question q = std::move(questions_.front());
    questions_.pop_front();
    questionShown_ = false;
    shownSince_ = -1;
    answeredThisStep_ = true;
    pending_.push_back({q.view.rect(), Waveform::Quality});
    log(LogLevel::Info, "confirmation %llu: %s", static_cast<unsigned long long>(q.id), *yes ? "yes" : "no");
    if (q.answered) guard([&] { q.answered(*yes); });
    return true;
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
    setSystemInk(false);  // the screen that wants it asks again
    if (lastVisible_ && lastVisible_ != visibleTop()) {
        Screen* was = lastVisible_;
        caller_ = was;
        guard([&] { was->onHide(*this); });
        caller_ = nullptr;
    }
    lastVisible_ = visibleTop();
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
    while (!stacks_[tab].empty()) {  // top first
        auto old = std::move(stacks_[tab].back());
        stacks_[tab].pop_back();
        retire(std::move(old));
    }
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
    auto s = std::move(stacks_[tab_].back());
    stacks_[tab_].pop_back();
    retire(std::move(s));
    showTop();
}

void App::switchTab(int index) {
    if (index < 0 || index >= kTabs || stacks_[index].empty()) return;
    if (index == tab_) {
        // Tapping the current tab goes back to its root.
        if (stacks_[tab_].size() > 1) {
            while (stacks_[tab_].size() > 1) {
                auto s = std::move(stacks_[tab_].back());
                stacks_[tab_].pop_back();
                retire(std::move(s));
            }
            showTop();
        }
        return;
    }
    // Screens that close when left (plugin screens) go, with everything above them.
    auto& stack = stacks_[tab_];
    size_t keep = stack.size();
    for (size_t i = 1; i < stack.size(); ++i)
        if (stack[i]->closesWhenHidden()) {
            keep = i;
            break;
        }
    while (stack.size() > keep) {
        auto s = std::move(stack.back());
        stack.pop_back();
        retire(std::move(s));
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

void App::paintNow(Rect r, Waveform w, const std::function<void(cairo_t*)>& paint) {
    if (caller_ && caller_ != visibleTop()) return;
    r = r.intersected({0, 0, canvas_.width(), canvas_.height()});
    if (r.empty()) return;
    cairo_t* cr = canvas_.cr();
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    paint(cr);
    cairo_restore(cr);
    if (w != Waveform::Ink) {
        display_.present(canvas_, r, w);
        return;
    }
    inkDirty_ = inkDirty_.united(r);
    presentInk(false);
}

void App::setSystemInk(bool on) {
    if (on && confirmShown()) return;  // no wet ink over the app's question
    if (on && (background_ || (caller_ && caller_ != visibleTop()))) return;
    if (on == systemInkOn_) return;
    if (display_.setSystemInk(on)) systemInkOn_ = on;
    else inkAskFailed_ = true;  // asked again on the next tick, which comes soon (nextTimeoutMs)
}

void App::presentInk(bool force) {
    if (inkDirty_.empty()) return;
    const int64_t now = clock_();
    if (!force && now - lastInkPresentMs_ < kInkFrameMs) return;
    display_.present(canvas_, inkDirty_, Waveform::Ink);
    inkDirty_ = {};
    lastInkPresentMs_ = now;
}

// Forget what the pen was doing (its Leave or Up may never come): no palm blocking, and a pen
// stroke used as a finger is lifted.
void App::penReset(std::vector<Gesture>& gestures) {
    penNear_ = false;
    penDown_ = false;
    penOwned_ = false;  // the screen had onBackground; no lift for it later
    if (penAsTouch_) {
        penAsTouch_ = false;
        auto g = gestures_.feed(TouchEvent{TouchEvent::Type::Up, kPenFinger, penTouchAt_, clock_()});
        gestures.insert(gestures.end(), g.begin(), g.end());
    }
}

void App::handlePen(const PenEvent& p, std::vector<Gesture>& gestures) {
    penLastMs_ = clock_();
    if (p.type == PenEvent::Type::Down) penDown_ = true;
    if (p.type == PenEvent::Type::Up || p.type == PenEvent::Type::Leave) penDown_ = false;
    if (p.type == PenEvent::Type::Leave) {
        penNear_ = false;
        penLeftMs_ = p.t_ms ? p.t_ms : clock_();
    } else {
        penNear_ = true;
    }
    Screen* s = visibleTop();
    if (!s) return;
    if (confirmShown()) {  // the pen never answers it, nor writes under it
        if (penAsTouch_) {  // a stroke that began before it: lifted (its tap goes nowhere)
            penAsTouch_ = false;
            auto g = gestures_.feed(TouchEvent{TouchEvent::Type::Up, kPenFinger, penTouchAt_, clock_()});
            gestures.insert(gestures.end(), g.begin(), g.end());
        }
        return;
    }
    bool took = false;
    caller_ = s;
    guard([&] { took = s->onPen(p, *this); });
    caller_ = nullptr;
    if (p.type == PenEvent::Type::Down) penOwned_ = took;
    if (p.type == PenEvent::Type::Up || p.type == PenEvent::Type::Leave) penOwned_ = false;
    penAt_ = p.pos;
    penEraser_ = p.eraser;
    if (p.type == PenEvent::Type::Down) penAsTouch_ = !took;  // decided for the whole stroke
    if (!penAsTouch_ || p.type == PenEvent::Type::Near) return;
    TouchEvent t{TouchEvent::Type::Move, kPenFinger, p.pos, p.t_ms ? p.t_ms : clock_()};
    penTouchAt_ = p.pos;
    if (p.type == PenEvent::Type::Down) t.type = TouchEvent::Type::Down;
    if (p.type == PenEvent::Type::Up || p.type == PenEvent::Type::Leave) {
        t.type = TouchEvent::Type::Up;
        penAsTouch_ = false;
    }
    auto g = gestures_.feed(t);
    gestures.insert(gestures.end(), g.begin(), g.end());
}

// A finger that lands while the pen is near (or just left) is the palm: it and its later samples
// are dropped. Fingers already down when the pen came keep going (the gesture must end).
bool App::palmBlocked(const TouchEvent& t) {
    auto it = std::find(palmFingers_.begin(), palmFingers_.end(), t.id);
    if (t.type == TouchEvent::Type::Down) {
        // Near only while it keeps reporting: a Leave that never came must not block touch forever.
        const bool near = penNear_ && clock_() - penLastMs_ < (penDown_ ? kPenDownStaleMs : kPenStaleMs);
        const bool palm = near || (penLeftMs_ >= 0 && t.t_ms - penLeftMs_ < kPalmMs);
        if (palm && it == palmFingers_.end()) palmFingers_.push_back(t.id);
        if (!palm && it != palmFingers_.end()) palmFingers_.erase(it);
        return palm;
    }
    if (it == palmFingers_.end()) return false;
    if (t.type == TouchEvent::Type::Up) palmFingers_.erase(it);
    return true;
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
    if (questionTakes(g)) return;
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

void App::wakeAt(int64_t ms) {
    if (wakeAt_ < 0 || ms < wakeAt_) wakeAt_ = ms;
}

int App::nextTimeoutMs() const {
    if (!pending_.empty() || !deferred_.empty()) return 0;  // asked for outside a step (main's toasts)
    const int64_t now = clock_();
    int64_t due = -1;
    auto at = [&](int64_t ms) {
        if (ms >= 0 && (due < 0 || ms < due)) due = ms;
    };
    at(wakeAt_);
    at(gestures_.nextDueMs());
    if (!inkDirty_.empty()) at(lastInkPresentMs_ + kInkFrameMs);
    if (toastShown_) at(toastUntil_);
    // A question waiting for a page turn to land or the pen to lift: looked at every step.
    if (!questions_.empty() && !questionShown_ && !background_) at(now + kBusyMs);
    if (inkAskFailed_) at(now + kBusyMs);
    // The storage check, while in front (asleep or behind the Kindle's home, nothing is written:
    // Foreground checks at once).
    if (watchdog_ && !background_) at(lastWatchdogMs_ + watchdogPeriodMs_);
    if (due < 0) return -1;
    return static_cast<int>(std::clamp<int64_t>(due - now, 0, INT_MAX));
}

void App::step(int timeout_ms) {
    beat("system ink");
    answeredThisStep_ = false;
    inkAskFailed_ = false;  // set again by a request the display doesn't do, from here on
    // Wet ink on where no screen takes handwriting (a pause that failed, e.g. the handler was
    // busy): ask again, whichever screen is showing.
    if (systemInkOn_) {
        const Screen* top = visibleTop();
        if (background_ || !top || !top->wantsSystemInk()) setSystemInk(false);
    }
    std::vector<InputEvent> events;
    if (!inkDirty_.empty()) timeout_ms = timeout_ms < 0 ? static_cast<int>(kInkFrameMs) : std::min<int>(timeout_ms, static_cast<int>(kInkFrameMs));
    beat("idle");  // waiting: the stall watchdog sleeps too
    if (!display_.pollEvents(events, timeout_ms)) {
        closed_ = true;
        return;
    }
    beat("input");
    wakeAt_ = -1;  // asked again, during this step, by whatever still waits for time
    std::vector<Gesture> gestures;
    for (const InputEvent& e : events) {
        switch (e.kind) {
            case InputEvent::Kind::Touch: {
                if (palmBlocked(e.touch)) break;
                auto g = gestures_.feed(e.touch);
                gestures.insert(gestures.end(), g.begin(), g.end());
                break;
            }
            case InputEvent::Kind::Key:
                if (confirmShown()) break;  // it waits for its answer
                if (Screen* s = visibleTop()) {
                    caller_ = s;
                    guard([&] { s->onKey(e.key, *this); });
                    caller_ = nullptr;
                }
                break;
            case InputEvent::Kind::Background:
                log(LogLevel::Info, "background");
                setSystemInk(false);  // a system dialog over us: no ink, and the pen is not ours
                background_ = true;
                penReset(gestures);
                if (Screen* s = visibleTop()) {
                    caller_ = s;
                    guard([&] { s->onBackground(*this); });
                    caller_ = nullptr;
                }
                if (lifecycle_) lifecycle_(false);
                break;
            case InputEvent::Kind::Resize:
                resized();
                break;
            case InputEvent::Kind::Pen:
                if (!background_) handlePen(e.pen, gestures);
                else if (e.pen.type == PenEvent::Type::Leave) penNear_ = penDown_ = penOwned_ = false;
                break;
            case InputEvent::Kind::Foreground:
                log(LogLevel::Info, "foreground");
                background_ = false;
                penReset(gestures);
                // Once, with a flash: not again where the display has just done so (the Kindle's
                // Expose after the screensaver), which would flash the whole screen twice.
                redrawAll(display_.repaintsOnForeground() ? Waveform::Quality : Waveform::Full);
                lastWatchdogMs_ = clock_() - watchdogPeriodMs_;  // the storage check, now (not run behind)
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
    if (pollHook_) {
        beat("plugin events");
        caller_ = nullptr;
        guard(pollHook_);
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
    showQuestion();  // after the tick: a page turn may just have landed
    auto deferred = std::move(deferred_);
    deferred_.clear();
    for (auto& f : deferred) guard(f);
    beat("drawing");
    presentInk(false);
    flush();
    while (!graveyard_.empty()) graveyard_.pop_back();  // newest (topmost) first
    beat("idle");
}

void App::flush() {
    if (pending_.empty() || !visibleTop()) return;
    const Rect all{0, 0, canvas_.width(), canvas_.height()};
    Rect fast, ui, quality, full;
    for (const auto& [r, w] : pending_) {
        Rect& u = w == Waveform::Fast || w == Waveform::Ink ? fast : w == Waveform::Ui ? ui : w == Waveform::Quality ? quality : full;
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
    const PluginConfirm* question = confirmShown();
    if (question) question->draw(canvas_.cr());
    if (!full.empty()) {
        const Rect shown = full.united(quality).united(ui).united(fast);
        display_.present(canvas_, shown, Waveform::Full);
        if (shown.united(inkDirty_) == shown) inkDirty_ = {};  // already on screen, in full quality
    } else {
        if (!quality.empty() && quality.united(inkDirty_) == quality) inkDirty_ = {};
        if (!fast.empty()) display_.present(canvas_, fast, Waveform::Fast);
        if (!ui.empty()) display_.present(canvas_, ui, Waveform::Ui);
        if (!quality.empty()) display_.present(canvas_, quality, Waveform::Quality);
    }
    if (question && shownSince_ < 0) shownSince_ = clock_();  // its tap guard counts from now
}

int App::run() {
    while (!quit_ && !closed_ && visibleTop()) step(nextTimeoutMs());
    return exitCode_;
}

}  // namespace zk
