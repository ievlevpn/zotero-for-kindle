#pragma once
// Plugins' background tasks after a sync, and the keep-awake hold around them.
//
// The sync worker never waits for plugins. When a sync has finished without error (SyncFinished,
// seen on the screen thread through App's event hook) and the Kindle is awake, the screen thread
// takes a KeepAwake hold of its own and queues PluginHost::runBackground(); the sync worker's
// hold has ended by then, and the Kindle doesn't sleep in the instant between. The hold ends at
// the first of:
//  - the host's BackgroundDone event (every queued round posts exactly one, once its tasks have
//    returned, failed or been stopped), via done();
//  - the cap, 35 s (the tasks' own budget is 30 s), via tick() from the poll hook;
//  - the Kindle going to sleep, via onSleep(), which also cuts the tasks short (cancel: not
//    counted as failures, retried next time).
// Nothing here blocks: a stuck plugin thread costs at most the cap.
// Screen thread only.

#include <cstdint>
#include <functional>

#include "platform/power.h"
#include "sync/sync_control.h"

namespace zk {

class BackgroundRound {
public:
    static constexpr int64_t kCapMs = 35'000;

    // `run`: queue the host's background round; `cancel`: cut a running one short.
    BackgroundRound(KeepAwake&, std::function<void()> run, std::function<void()> cancel);
    ~BackgroundRound();
    BackgroundRound(const BackgroundRound&) = delete;
    BackgroundRound& operator=(const BackgroundRound&) = delete;

    // Starts a round after a SyncFinished without error while `awake`; anything else is ignored.
    // A round already under way is extended to a fresh cap, not doubled.
    void onSyncEvent(const SyncEvent&, bool awake, int64_t nowMs);
    void done();                 // the host reports the round done
    void tick(int64_t nowMs);    // releases at the cap
    void onSleep();              // cancels a running round and releases
    bool holding() const { return holding_; }

private:
    void release();
    KeepAwake& keep_;
    std::function<void()> run_, cancel_;
    bool holding_ = false;
    int64_t endsAt_ = 0;
};

}  // namespace zk
