#pragma once
// What the plugin host tells the screen thread, routed each loop iteration (App::setPollHook):
//  - Confirm: the app's own confirmation over whatever shows (App::confirm); the answer goes back
//    to the host, once.
//  - Withdrawn: that question no longer waits (its call was stopped, or the plugin turned off):
//    it goes away unanswered.
//  - ChangesQueued: a plugin queued a change for Zotero: the sync pushes soon.
//  - BackgroundDone: the background round ended: onBackgroundDone() (the keep-awake hold, see
//    plugin_background.h).
//  - Disabled: a toast "<Plugin> stopped working — turned off" (the host's text); while a system
//    dialog or the screensaver is over the app it waits until the app is in front again; several
//    are shown one after another.
//  - Badge, Log: nothing here (Settings → Plugins reads badges and logs from the host).

#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "plugins/host.h"
#include "sync/sync_control.h"

namespace zk {

class App;

// What a sync tells plugins: the changes Zotero made (a push that had to pull first reports its
// own), then, when a sync finished without error, syncFinished {origin "zotero", initial}: after
// the other events of that sync. The first full sync of a library carries no per-item events.
void forwardSyncToPlugins(const SyncEvent&, PluginSink&);

class PluginEvents {
public:
    explicit PluginEvents(PluginHost*);  // nullptr: no host (nothing to poll)
    void poll(App&);
    // Another host (or none) from now on, e.g. the old one stopped for an update's install: the
    // old one's questions are taken away unanswered, its waiting events dropped.
    void rebind(PluginHost*, App&);

    // Questions shown or waiting to be shown, not answered or withdrawn yet.
    void onBackgroundDone(std::function<void(uint64_t round)> f) { backgroundDone_ = std::move(f); }

    int waiting() const { return static_cast<int>(open_.size()); }

    static constexpr int64_t kToastGapMs = 3000;  // a toast's time on screen

private:
    PluginHost* host_;
    std::set<uint64_t> open_;
    std::deque<std::string> turnedOff_;  // toasts not shown yet
    int64_t lastToastMs_ = -kToastGapMs;
    std::function<void(uint64_t)> backgroundDone_;
};

}  // namespace zk
