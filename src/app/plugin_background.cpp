#include "app/plugin_background.h"

namespace zk {

BackgroundRound::BackgroundRound(KeepAwake& keep, std::function<void()> run, std::function<void()> cancel)
    : keep_(keep), run_(std::move(run)), cancel_(std::move(cancel)) {}

BackgroundRound::~BackgroundRound() { release(); }

void BackgroundRound::release() {
    if (!holding_) return;
    holding_ = false;
    keep_.release();
}

void BackgroundRound::onSyncEvent(const SyncEvent& e, bool awake, int64_t nowMs) {
    if (e.kind != SyncEvent::Kind::SyncFinished || e.error || !awake) return;
    if (!holding_) {
        keep_.acquire();
        holding_ = true;
    }
    endsAt_ = nowMs + kCapMs;
    run_();
}

void BackgroundRound::done() { release(); }

void BackgroundRound::tick(int64_t nowMs) {
    if (holding_ && nowMs >= endsAt_) release();
}

void BackgroundRound::onSleep() {
    if (!holding_) return;
    cancel_();
    release();
}

}  // namespace zk
