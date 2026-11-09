#pragma once
// The desktop developer loop: watches <pluginsDir>/<id>/main.js and asks for that plugin to be
// reloaded when the file's modification time changes. poll() is cheap and may be called as often
// as you like; it looks at the disk at most once a second. Screen thread (or any one thread).

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "plugins/host.h"

namespace zk {

// --enable-plugin <id> (developer mode, repeatable): turns these plugins on in the --plugins-dir
// folder's state, as the author's deliberate choice (the same as Turn on in Settings → Plugins).
// Returns a line for each id that isn't a folder there; a folder that can't load stays off and
// devLoopNotes says why.
std::vector<std::string> enablePluginsForDev(PluginHost& host, const std::vector<std::string>& ids);

// Developer mode: one line for each plugin that won't load, saying why and what to do, e.g.
// "plugin hello: off — turn it on in Settings → Plugins, or start with --enable-plugin hello".
std::vector<std::string> devLoopNotes(const std::vector<PluginStatus>& plugins, const Version& app);

class PluginWatcher {
public:
    PluginWatcher(std::string pluginsDir, std::function<void(const std::string& id)> reload);
    void poll(int64_t nowMs);

private:
    std::string dir_;
    std::function<void(const std::string&)> reload_;
    std::map<std::string, int64_t> seen_;  // id → main.js mtime (ns)
    int64_t lastMs_ = 0;
    bool started_ = false;
};

}  // namespace zk
