#include "app/plugin_watch.h"

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
        if (!isNew && pos->second != mtime) {
            pos->second = mtime;
            if (!first) reload_(id);
        }
    }
}

}  // namespace zk
