#pragma once
// A plugin's log, <plugin dir>/plugin.log: what its console.* wrote and what the host has to say
// about it (load errors, failures, stops). Rotated at a size limit: the full file becomes
// plugin.log.1 (replacing the previous one), so a plugin's logs take at most twice the limit.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace zk::plugins {

enum class LogLevel { Info, Warn, Error };

class PluginLog {
public:
    static constexpr size_t kMaxBytes = 256 * 1024;
    static constexpr size_t kMaxEntry = 16 * 1024;  // one entry longer than this is cut

    // `clock`: wall time in ms since the epoch, for the time stamps.
    PluginLog(std::string path, size_t maxBytes, std::function<int64_t()> clock);
    ~PluginLog();
    PluginLog(const PluginLog&) = delete;
    PluginLog& operator=(const PluginLog&) = delete;

    // Writes "YYYY-MM-DD HH:MM:SS [warn] text" (no tag for Info) and returns that line.
    std::string write(LogLevel, std::string text);

private:
    bool open();
    std::string path_;
    size_t max_;
    std::function<int64_t()> clock_;
    FILE* f_ = nullptr;
    size_t size_ = 0;
};

}  // namespace zk::plugins
