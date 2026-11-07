// A stuck UI must never need a reboot: when the loop stops, the watchdog ends the app (the
// launcher then returns to the Kindle home) and says in the log what it was doing.

#include <doctest/doctest.h>

#include "app/stall_watchdog.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using namespace zk;

TEST_CASE("stall watchdog: stays quiet while the loop beats") {
    std::atomic<int> stalls{0};
    StallWatchdog w(150, [&](int64_t, const std::string&) { ++stalls; });
    for (int i = 0; i < 30; ++i) {
        w.beat("idle");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(stalls == 0);
}

TEST_CASE("stall watchdog: fires once when the loop stops, naming what it was doing") {
    std::mutex m;
    std::string doing;
    int64_t stalledMs = 0;
    std::atomic<int> stalls{0};
    StallWatchdog w(150, [&](int64_t ms, const std::string& d) {
        std::lock_guard<std::mutex> lock(m);
        doing = d;
        stalledMs = ms;
        ++stalls;
    });
    w.beat("gesture");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(stalls == 1);
    std::lock_guard<std::mutex> lock(m);
    CHECK(doing == "gesture");
    CHECK(stalledMs >= 150);
}
