#include "app/stall_watchdog.h"

#include <algorithm>
#include <chrono>

namespace zk {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

StallWatchdog::StallWatchdog(int timeoutMs, OnStall onStall)
    : timeoutMs_(timeoutMs), onStall_(std::move(onStall)), lastBeatMs_(nowMs()), thread_([this] { run(); }) {}

StallWatchdog::~StallWatchdog() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void StallWatchdog::beat(const char* doing) {
    doing_ = doing;
    lastBeatMs_ = nowMs();
}

void StallWatchdog::run() {
    const auto every = std::chrono::milliseconds(std::clamp(timeoutMs_ / 4, 10, 1000));
    bool fired = false;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!cv_.wait_for(lock, every, [this] { return stop_; })) {
        const int64_t stalled = nowMs() - lastBeatMs_;
        if (stalled < timeoutMs_) {
            fired = false;
            continue;
        }
        if (fired) continue;
        fired = true;
        lock.unlock();
        onStall_(stalled, doing_.load());
        lock.lock();
    }
}

}  // namespace zk
