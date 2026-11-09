#include "app/plugin_watch.h"

#include <algorithm>
#include <filesystem>

namespace zk {

namespace fs = std::filesystem;

PluginWatcher::PluginWatcher(std::string pluginsDir, std::function<void(const std::string&)> reload)
    : dir_(std::move(pluginsDir)), reload_(std::move(reload)) {}

void PluginWatcher::poll(int64_t nowMs) {
    if (started_ && nowMs - lastMs_ < 1000) return;
    lastMs_ = nowMs;
    const bool first = !started_;
    started_ = true;
    std::error_code ec;
    for (fs::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string id = it->path().filename().string();
        if (id.empty() || id[0] == '.') continue;
        std::error_code e2;
        const auto t = fs::last_write_time(it->path() / "main.js", e2);
        if (e2) continue;
        const int64_t mtime = t.time_since_epoch().count();
        auto [pos, isNew] = seen_.emplace(id, mtime);
        if (isNew ? !first : pos->second != mtime) {  // a folder that appears later is reloaded: loads if enabled
            pos->second = mtime;
            reload_(id);
        }
    }
}

std::vector<std::string> enablePluginsForDev(PluginHost& host, const std::vector<std::string>& ids) {
    std::vector<std::string> notes;
    const auto plugins = host.status();
    for (const auto& id : ids) {
        const bool listed = std::any_of(plugins.begin(), plugins.end(), [&](const PluginStatus& s) { return s.folder.dir == id; });
        if (listed) host.setEnabled(id, true);  // refused (stays off) for a folder that can't load
        else notes.push_back("plugin " + id + ": --enable-plugin found no folder \"" + id + "\" in the plugins folder");
    }
    return notes;
}

std::vector<std::string> devLoopNotes(const std::vector<PluginStatus>& plugins, const Version& app) {
    std::vector<std::string> notes;
    for (const auto& s : plugins) {
        const std::string& id = s.folder.dir;
        const auto& f = s.folder;
        if (f.incompatible && f.otherApi)
            notes.push_back("plugin " + id + ": incompatible — written for plugin API " + std::to_string(f.otherApi) +
                            " (this app runs API 0)");
        else if (f.incompatible && f.manifest)
            notes.push_back("plugin " + id + ": incompatible — needs app " + f.manifest->minApp.str() + " (this app is " +
                            app.str() + "); lower minApp in plugin.json to run it here");
        else if (!f.problem.empty())
            notes.push_back("plugin " + id + ": can't load — " + f.problem);
        else if (!s.enabled)
            notes.push_back("plugin " + id + ": off — turn it on in Settings → Plugins, or start with --enable-plugin " + id);
    }
    return notes;
}

}  // namespace zk
