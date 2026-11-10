#pragma once
// Waking the UI loop from other threads (battery). The loop sleeps in Display::pollEvents until
// input comes, a deadline it asked for passes, or a worker calls wakeUi() after leaving a result
// for it (a sync event, a rendered page, a plugin's answer, the catalog's or updater's news).
// Without wakes the loop would have to poll every queue a few times a second, which kept the
// Kindle's CPU out of deep idle all day.
//
// Wakes are coalesced: the first wakeUi() after the display took the last one calls the waker
// (a write to the display's self-pipe, an SDL user event); more before the next take do nothing.

namespace zk {

using UiWaker = void (*)();

// The display installs it (null: none; wakes are then only remembered). Any thread.
void setUiWaker(UiWaker);
// After queuing something for the UI thread. Any thread; never blocks; cheap.
void wakeUi();
// The display, on the UI thread: is a wake waiting (then don't sleep)?
bool uiWakePending();
// The display, on the UI thread, once it stops waiting and before the loop looks at the queues:
// the wakes so far are taken (a later one wakes the next wait).
void uiWakeTaken();

}  // namespace zk
