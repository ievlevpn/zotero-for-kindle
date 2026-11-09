#pragma once
// What the user and the app remember about each plugin: on/off, failures, a badge, the last
// background run. One file, <pluginsDir>/.state.json, written atomically; a torn or garbled file
// reads as defaults.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace zk {

class PluginState {
public:
    explicit PluginState(std::string file);

    bool enabled(const std::string& id);  // false for a plugin never seen
    void setEnabled(const std::string& id, bool on);
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
        bool enabled = false;
        int failures = 0;
        std::vector<std::string> errors;
        std::string badge;
        int64_t lastBackgroundMs = 0;
    };
    void load();
    void save();
    std::string file_;
    std::map<std::string, Entry> entries_;
};

}  // namespace zk
