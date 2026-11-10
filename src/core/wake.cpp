#include "core/wake.h"

#include <atomic>

namespace zk {

namespace {

std::atomic<UiWaker> gWaker{nullptr};
std::atomic<bool> gPending{false};

}  // namespace

void setUiWaker(UiWaker f) { gWaker.store(f); }

void wakeUi() {
    if (gPending.exchange(true)) return;  // the display hasn't taken the last one yet
    if (UiWaker f = gWaker.load()) f();
}

bool uiWakePending() { return gPending.load(); }

void uiWakeTaken() { gPending.store(false); }

}  // namespace zk
