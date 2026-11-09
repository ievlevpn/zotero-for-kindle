#pragma once
// The plugin host: one plugin thread running every enabled plugin, each in its own QuickJS runtime
// and context. The screen thread never waits on JavaScript: it queues commands (dispatch,
// runBackground, answer, reload, setEnabled) and polls PluginHostEvents, like sync and render
// events. See plugin_ctx.h for how bindings (zk.*) plug in.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugins/manifest.h"
#include "update/version.h"

namespace zk {

class Http;

namespace plugins {
struct PluginCtx;
}

struct PluginHostDeps {
    std::string pluginsDir, dbPath;
    // Names the plugin whose code is running (written before a plugin's code runs, removed when
    // the plugin thread goes idle). Empty: no marker.
    std::string crashMarker;
    Version appVersion;
    std::function<std::unique_ptr<Http>()> makeHttp;
    std::string webdavHost;
    std::function<int64_t()> clock;  // wall clock, ms since the epoch (log times); default: system
    size_t memoryPerPlugin = 32 << 20;
    // What all plugins together may use (actual use, shared): an allocation that would pass it
    // fails, stopping the call that made it.
    size_t memoryTotal = 96u << 20;
    int handlerBudgetMs = 5000, backgroundBudgetMs = 30000;
    // Each line written to a plugin's log is also handed here (on the plugin thread): the desktop
    // build mirrors it to stderr when asked to.
    std::function<void(const std::string& pluginId, const std::string& line)> logSink;
    // More bindings installed into every plugin's `zk` after the built-in ones (tests use this to
    // give plugins host work they control).
    std::vector<std::function<void(plugins::PluginCtx&)>> extraBindings;
};

struct PluginEvent {
    std::string name;
    nlohmann::json payload;
    bool userCaused = false;
};

struct PluginHostEvent {
    enum class Kind { Confirm, Badge, Disabled, ChangesQueued, Log } kind;
    std::string pluginId, pluginName, text;
    std::vector<std::string> lines;
    uint64_t requestId = 0;
};

struct PluginStatus {
    PluginFolder folder;
    bool enabled = false, loaded = false;
    std::string badge;
    std::vector<std::string> errors;  // newest last: past failures, then a current problem
};

class PluginHost {
public:
    // Scans the plugins folder (nothing runs yet). If the crash marker names a plugin, the app
    // stopped while it was running: it is turned off, with a note, and the marker removed.
    explicit PluginHost(PluginHostDeps);
    ~PluginHost();  // stop()
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Starts the plugin thread. Plugins load lazily: each enabled plugin when the first event or
    // background run is due. Commands queued before start() wait for it.
    void start();
    // Interrupts whatever plugin code is running and joins the thread; returns within 2 s. Calls
    // cut short by stop() are not counted as failures.
    void stop();
    // Unloads and rescans; plugins that were loaded are loaded again at once.
    void reload();
    void reload(const std::string& id);
    void dispatch(PluginEvent);
    void runBackground();
    // The user's answer to a Confirm event.
    void answer(uint64_t requestId, bool yes);
    bool poll(PluginHostEvent&);
    std::vector<PluginStatus> status() const;
    // Turning a plugin on clears its failures; turning it off unloads it.
    void setEnabled(const std::string& id, bool);

    // Blocks until no command is queued and no call is running or awaiting (timers may be
    // pending), or `timeoutMs` passes. True if idle. For tests and the background keep-awake hold.
    bool waitIdle(int timeoutMs);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace zk
