#pragma once
// What the plugin host tells the screen thread, routed each loop iteration (App::setPollHook):
//  - Confirm: the app's own confirmation over whatever shows (App::confirm); the answer goes back
//    to the host, once.
//  - Withdrawn: that question no longer waits (its call was stopped, or the plugin turned off):
//    it goes away unanswered.
//  - ChangesQueued: a plugin queued a change for Zotero: the sync pushes soon.
//  - Badge, Disabled, Log: kept for Settings → Plugins (Task 17).

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "plugins/host.h"

namespace zk {

class App;

class PluginEvents {
public:
    explicit PluginEvents(PluginHost*);  // nullptr: no host (nothing to poll)
    void poll(App&);
    // Another host (or none) from now on, e.g. the old one stopped for an update's install: the
    // old one's questions are taken away unanswered, its waiting events dropped.
    void rebind(PluginHost*, App&);

    // Questions shown or waiting to be shown, not answered or withdrawn yet.
    int waiting() const { return static_cast<int>(open_.size()); }

    struct Notice {
        PluginHostEvent::Kind kind;
        std::string pluginId, pluginName, text;
    };
    static constexpr size_t kNotices = 50;
    // Disabled and Log events, oldest first (the newest kNotices).
    const std::vector<Notice>& notices() const { return notices_; }
    // Each plugin's badge ("" once cleared).
    const std::map<std::string, std::string>& badges() const { return badges_; }

private:
    PluginHost* host_;
    std::set<uint64_t> open_;
    std::vector<Notice> notices_;
    std::map<std::string, std::string> badges_;
};

}  // namespace zk
