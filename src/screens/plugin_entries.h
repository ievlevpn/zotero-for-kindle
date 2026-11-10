#pragma once
// Where plugins show up in the app's own screens (the reader's selection bar, an item's details
// page, Home's launcher icon), as those screens see it: what the enabled, running plugins
// registered, and running their actions. The app's PluginEvents implements it over the plugin
// host; Nav::pluginEntries() is null when there is none (tests, no host), and then no screen
// shows any of it.
//
// Nothing here waits on JavaScript: run() only queues; the answer is taken later with take().

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zk {

struct PluginAction {
    std::string pluginId, pluginName, id, title;
};

struct PluginLaunch {  // a plugin's main screen, for the launcher
    std::string pluginId, pluginName, screenId, title, badge;
};

// What a run came to: the pop-up's text and buttons (id, title), or an error ("<Plugin> couldn't
// do that: …"), or nothing to show (`none`: run returned nothing).
struct ActionOutcome {
    bool error = false, none = false;
    std::string text;
    std::vector<std::pair<std::string, std::string>> buttons;
};

class PluginEntries {
public:
    virtual ~PluginEntries() = default;
    // kind "selectionAction" or "itemAction": every enabled, running plugin's, in plugin order.
    virtual std::vector<PluginAction> actions(const std::string& kind) = 0;
    virtual std::vector<PluginLaunch> mainScreens() = 0;
    virtual bool anyBadge() = 0;
    // Changes whenever what the three above return may have changed (cheap: for onTick).
    virtual uint64_t version() = 0;
    // Starts `action` on `target`; `again` {action, text} (a pop-up button) or null. Returns its number.
    virtual uint64_t run(const PluginAction&, const std::string& kind, nlohmann::ordered_json target,
                         nlohmann::json again = nullptr) = 0;
    // Its outcome, once, when it has come.
    virtual std::optional<ActionOutcome> take(uint64_t run) = 0;
    // The user closed what waited for it: the run is stopped.
    virtual void stop(uint64_t run) = 0;
    // Pushes the plugin's screen (the launcher).
    virtual void openScreen(const std::string& pluginId, const std::string& screenId) = 0;
};

}  // namespace zk
