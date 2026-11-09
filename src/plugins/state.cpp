#include "plugins/state.h"

#include <unistd.h>

#include <filesystem>
#include <atomic>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace zk {

using nlohmann::json;

PluginState::PluginState(std::string file) : file_(std::move(file)) {
    std::lock_guard<std::mutex> lock(mu_);
    load();
}

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
    static std::atomic<unsigned> counter{0};  // unique per write, so two states never share a temp file
    std::string tmp = file_ + "." + std::to_string(::getpid()) + "." + std::to_string(counter++) + ".tmp";
    try {
        std::string text = json{{"plugins", plugins}}.dump(1, ' ', false, json::error_handler_t::replace);
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << text;
        out.flush();
        if (!out) { out.close(); std::error_code rm; std::filesystem::remove(tmp, rm); return; }
    } catch (const std::exception&) {
        std::error_code rm;
        std::filesystem::remove(tmp, rm);
        return;  // keep running; the change stays in memory
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file_, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

bool PluginState::enabled(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.enabled;
}
void PluginState::setEnabled(const std::string& id, bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[id].enabled = on;
    save();
}
int PluginState::failures(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? 0 : it->second.failures;
}
void PluginState::recordFailure(const std::string& id, std::string message) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = entries_[id];
    e.failures++;
    e.errors.push_back(std::move(message));
    if (e.errors.size() > 5) e.errors.erase(e.errors.begin(), e.errors.end() - 5);
    save();
}
void PluginState::clearFailures(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = entries_[id];
    e.failures = 0;
    save();
}
std::vector<std::string> PluginState::lastErrors(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? std::vector<std::string>{} : it->second.errors;
}
std::string PluginState::badge(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? "" : it->second.badge;
}
void PluginState::setBadge(const std::string& id, std::string badge) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[id].badge = std::move(badge);
    save();
}
int64_t PluginState::lastBackgroundMs(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? 0 : it->second.lastBackgroundMs;
}
void PluginState::setLastBackgroundMs(const std::string& id, int64_t ms) {
    std::lock_guard<std::mutex> lock(mu_);
    entries_[id].lastBackgroundMs = ms;
    save();
}

}  // namespace zk
