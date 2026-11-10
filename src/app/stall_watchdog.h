#pragma once
// The escape path for a stuck app (Pavel: a freeze must never need a reboot). The UI loop beats
// with a word for what it is doing; if no beat comes for `timeoutMs`, a thread of its own calls
// onStall once — in the app: log it and end the process, so the launcher returns to the Kindle
// home. steady_clock does not run while the Kindle sleeps, so sleep is not a stall.
// Nor is waiting: the loop beats "idle" before it waits for input, which may take hours now
// that it sleeps until something is due (App::nextTimeoutMs). While idle the watchdog's thread
// sleeps too (no timer), and the next beat starts the count again.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace zk {

class StallWatchdog {
public:
    using OnStall = std::function<void(int64_t stalledMs, const std::string& doing)>;
    StallWatchdog(int timeoutMs, OnStall onStall);
    ~StallWatchdog();
    StallWatchdog(const StallWatchdog&) = delete;
    StallWatchdog& operator=(const StallWatchdog&) = delete;

    void beat(const char* doing);  // `doing`: a string literal; "idle": waiting, never a stall

private:
    void run();

    const int timeoutMs_;
    OnStall onStall_;
    std::atomic<int64_t> lastBeatMs_;
    std::atomic<const char*> doing_{"starting"};
    std::atomic<bool> idle_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace zk
