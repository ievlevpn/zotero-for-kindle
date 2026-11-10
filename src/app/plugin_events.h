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
//  - Screens: openScreen() pushes a PluginScreen; ScreenOpen (zk.ui.open) pushes one over its
//    parent (if the parent is still open; else the host is told it went); trees, errors and
//    ScreenClose wait for their screen, which takes them when it is visible (PluginScreen::onTick).
//    rebind() gives every open plugin screen the error state.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "plugins/host.h"
#include "screens/plugin_screen.h"
#include "sync/sync_control.h"

namespace zk {

class App;

// What a sync tells plugins: the changes Zotero made (a push that had to pull first reports its
// own), then, when a sync finished without error, syncFinished {origin "zotero", initial}: after
// the other events of that sync. The first full sync of a library carries no per-item events.
void forwardSyncToPlugins(const SyncEvent&, PluginSink&);

class PluginEvents final : public ScreenLink {
public:
    explicit PluginEvents(PluginHost*);  // nullptr: no host (nothing to poll)
    void poll(App&);
    // Another host (or none) from now on, e.g. the old one stopped for an update's install: the
    // old one's questions are taken away unanswered, its waiting events dropped.
    void rebind(PluginHost*, App&);

    // Questions shown or waiting to be shown, not answered or withdrawn yet.
    void onBackgroundDone(std::function<void(uint64_t round)> f) { backgroundDone_ = std::move(f); }

    int waiting() const { return static_cast<int>(open_.size()); }

    // Whether plugins run (not in safe mode, not stopped for an update's install); set by main.
    void setRunning(bool on) { running_ = on; }
    // Opens a plugin's screen over what shows (the launcher, an entry point): returns its instance.
    // Plugins not running (or no host): the screen shows "<Plugin> can't show this screen right now".
    uint64_t openScreen(App&, const std::string& pluginId, const std::string& screenId, nlohmann::json params = nullptr);
    // ScreenLink (PluginScreen).
    void send(uint64_t instance, nlohmann::json event) override;
    void left(uint64_t instance) override;
    bool take(uint64_t instance, PluginHostEvent& out) override;

    static constexpr int64_t kToastGapMs = 3000;  // a toast's time on screen

private:
    PluginHost* host_;
    std::set<uint64_t> open_;
    std::deque<std::string> turnedOff_;  // toasts not shown yet
    int64_t lastToastMs_ = -kToastGapMs;
    std::function<void(uint64_t)> backgroundDone_;
    bool running_ = true;
    std::map<uint64_t, std::string> screens_;                // open plugin screens: instance → plugin name
    std::map<uint64_t, std::deque<PluginHostEvent>> mail_;   // what waits for them
    void pushScreen(App&, uint64_t instance, const std::string& pluginName, const std::string& title);
};

}  // namespace zk
