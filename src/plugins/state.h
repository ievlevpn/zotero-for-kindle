#pragma once
// What the user and the app remember about each plugin: on/off, failures, a badge, the last
// background run. Thread-safe (the screen and plugin threads share it). One file, <pluginsDir>/.state.json, written atomically; a torn or garbled file
// reads as defaults.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace zk {

class PluginState {
public:
    explicit PluginState(std::string file);

    bool enabled(const std::string& id);  // false for a plugin never seen
    // The user's choice: on also marks it ever enabled; either clears stopped.
    void setEnabled(const std::string& id, bool on);
    bool everEnabled(const std::string& id);
    // The app turned it off (it kept failing, or the app stopped while it ran).
    void stop(const std::string& id);
    bool stopped(const std::string& id);
    void forget(const std::string& id);  // everything about it (a removed plugin)
    int failures(const std::string& id);  // in a row
    void recordFailure(const std::string& id, std::string message);
    void clearFailures(const std::string& id);
    std::vector<std::string> lastErrors(const std::string& id);  // newest last, at most 5
    std::string badge(const std::string& id);
    void setBadge(const std::string& id, std::string badge);
    int64_t lastBackgroundMs(const std::string& id);
    void setLastBackgroundMs(const std::string& id, int64_t ms);

private:
    struct Entry {
        bool enabled = false, everEnabled = false, stopped = false;
        int failures = 0;
        std::vector<std::string> errors;
        std::string badge;
        int64_t lastBackgroundMs = 0;
    };
    void load();
    void save();
    std::mutex mu_;
    std::string file_;
    std::map<std::string, Entry> entries_;
};

}  // namespace zk
