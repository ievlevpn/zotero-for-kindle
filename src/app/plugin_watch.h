#pragma once
// The desktop developer loop: watches <pluginsDir>/<id>/main.js and asks for that plugin to be
// reloaded when the file's modification time changes. poll() is cheap and may be called as often
// as you like; it looks at the disk at most once a second. Screen thread (or any one thread).

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace zk {

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
