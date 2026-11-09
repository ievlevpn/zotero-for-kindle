#include "plugins/state.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace zk {

using nlohmann::json;

PluginState::PluginState(std::string file) : file_(std::move(file)) { load(); }

void PluginState::load() {
    entries_.clear();
    std::ifstream in(file_, std::ios::binary);
    if (!in) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        json j = json::parse(ss.str());
        for (auto it = j.at("plugins").begin(); it != j.at("plugins").end(); ++it) {
            Entry e;
            const json& v = it.value();
            e.enabled = v.value("enabled", false);
            e.failures = v.value("failures", 0);
            e.badge = v.value("badge", std::string());
            e.lastBackgroundMs = v.value("lastBackgroundMs", (int64_t)0);
            if (v.contains("errors"))
                for (const auto& s : v["errors"]) e.errors.push_back(s.get<std::string>());
            entries_[it.key()] = std::move(e);
        }
    } catch (const json::exception&) {
        entries_.clear();  // torn or garbled: start over
    }
}

void PluginState::save() {
    json plugins = json::object();
    for (const auto& [id, e] : entries_)
        plugins[id] = {{"enabled", e.enabled}, {"failures", e.failures}, {"errors", e.errors},
                       {"badge", e.badge}, {"lastBackgroundMs", e.lastBackgroundMs}};
    std::string tmp = file_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << json{{"plugins", plugins}}.dump(1);
        out.flush();
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

bool PluginState::enabled(const std::string& id) {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.enabled;
}
void PluginState::setEnabled(const std::string& id, bool on) {
    entries_[id].enabled = on;
    save();
}
int PluginState::failures(const std::string& id) {
    auto it = entries_.find(id);
    return it == entries_.end() ? 0 : it->second.failures;
}
void PluginState::recordFailure(const std::string& id, std::string message) {
    Entry& e = entries_[id];
    e.failures++;
    e.errors.push_back(std::move(message));
    if (e.errors.size() > 5) e.errors.erase(e.errors.begin(), e.errors.end() - 5);
    save();
}
void PluginState::clearFailures(const std::string& id) {
    Entry& e = entries_[id];
    e.failures = 0;
    save();
}
std::vector<std::string> PluginState::lastErrors(const std::string& id) {
    auto it = entries_.find(id);
    return it == entries_.end() ? std::vector<std::string>{} : it->second.errors;
}
std::string PluginState::badge(const std::string& id) {
    auto it = entries_.find(id);
    return it == entries_.end() ? "" : it->second.badge;
}
void PluginState::setBadge(const std::string& id, std::string badge) {
    entries_[id].badge = std::move(badge);
    save();
}
int64_t PluginState::lastBackgroundMs(const std::string& id) {
    auto it = entries_.find(id);
    return it == entries_.end() ? 0 : it->second.lastBackgroundMs;
}
void PluginState::setLastBackgroundMs(const std::string& id, int64_t ms) {
    entries_[id].lastBackgroundMs = ms;
    save();
}

}  // namespace zk
