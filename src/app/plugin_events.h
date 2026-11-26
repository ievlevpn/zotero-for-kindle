#pragma once
#include <optional>
// What the plugin host tells the screen thread, routed each loop iteration (App::setPollHook):
//  - Confirm: the app's own confirmation over whatever shows (App::confirm); the answer goes back
//    to the host, once.
//  - Withdrawn: that question no longer waits (its call was stopped, or the plugin turned off):
//    it goes away unanswered.
//  - ChangesQueued: a plugin queued a change for Zotero: the sync pushes soon.
//  - BackgroundDone: the background round ended: onBackgroundDone() (the keep-awake hold, see
//    plugin_background.h).
//  - Disabled: a toast "<Plugin> stopped working — turned off" (the host's text); while a system
//    dialog or the screensaver is over the app it waits until the app is in front again; several
//    are shown one after another.
//  - Badge, Log: nothing here (Settings → Plugins reads badges and logs from the host).
//  - Screens: openScreen() pushes a PluginScreen; ScreenOpen (zk.ui.open) pushes one over its
//    parent (if the parent is still open; else the host is told it went); trees, errors and
//    ScreenClose wait for their screen, which takes them when it is visible (PluginScreen::onTick);
//    of its trees only the newest waits (errors all do).
//    rebind() gives every open plugin screen the error state.
//  - Entry points (PluginEntries, for the reader's bar, item details and Home): what the enabled
//    plugins registered, only while plugins run; asking wakes the host once (plugins load lazily)
//    so their entries become known. ActionResult/ActionError wait for take(); a rebind answers
//    the runs still waiting with an error.
//  - ItemOpen (zk.library.open): the item opener (main) opens the item; the screen asking is told
//    when it shows again (ScreenLink::takeItemOpened) that the reader closed.
//  - ReaderOpen (zk.reader.open): the preview opener (main) shows it over what shows; when that
//    reader closes, a ReaderClosed waits for the plugin screen under it, which tells the plugin.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "plugins/host.h"
#include "screens/plugin_entries.h"
#include "screens/plugin_screen.h"
#include "sync/sync_control.h"

namespace zk {

class App;

// What a sync tells plugins: the changes Zotero made (a push that had to pull first reports its
// own), then, when a sync finished without error, syncFinished {origin "zotero", initial}: after
// the other events of that sync. The first full sync of a library carries no per-item events.
void forwardSyncToPlugins(const SyncEvent&, PluginSink&);

class PluginEvents final : public ScreenLink, public PluginEntries {
public:
    explicit PluginEvents(PluginHost*);  // nullptr: no host (nothing to poll)
    void poll(App&);
    // Another host (or none) from now on, e.g. the old one stopped for an update's install: the
    // old one's questions are taken away unanswered, its waiting events dropped.
    void rebind(PluginHost*, App&);

    // Questions shown or waiting to be shown, not answered or withdrawn yet.
    void onBackgroundDone(std::function<void(uint64_t round)> f) { backgroundDone_ = std::move(f); }

    int waiting() const { return static_cast<int>(open_.size()); }

    // Whether plugins run (not in safe mode, not stopped for an update's install); set by main.
    void setRunning(bool on) { running_ = on; }
    // While this answers true (an account switch: nothing may cover the Switching screen), what a
    // plugin would put on screen is refused: a screen it opens is closed, a question is answered
    // no, zk.library.open and zk.reader.open are rejected with kRefusedWhileSwitching; the
    // launcher opens nothing.
    void setRefusing(std::function<bool()> f) { refusing_ = std::move(f); }
    static constexpr const char* kRefusedWhileSwitching = "Switching accounts";
    // Previews (zk.reader.open, ReaderOpen): main opens the reader (PreviewScreen::show) with these
    // answers: `answer` settles the plugin's promise ("" opened, else why not); `closed` tells the
    // plugin's screen the reader closed (a {kind: "reader", closed: true, ...} event). Unset: refused.
    using PreviewOpener = std::function<void(App&, const PluginHostEvent&, std::function<void(const std::string& error)> answer,
                                             std::function<void(nlohmann::json)> closed)>;
    void setPreviewOpener(PreviewOpener f) { previewOpener_ = std::move(f); }
    // Library items (zk.library.open, ItemOpen): main resolves the ref (itemToOpen) and opens it the
    // app's way (OpenFlow), at `annotation` if given. `answer` settles the plugin's promise ("" opened,
    // else why not: "Item not found", "Annotation not found", "Cancelled", a download's error), "" once
    // the reader shows; the asking screen is then told when it shows again that the reader closed.
    // Unset: refused.
    using ItemOpener = std::function<void(App&, const ItemRef&, std::optional<std::string> attachment,
                                          std::optional<std::string> annotation, std::function<void(const std::string& error)> answer)>;
    void setItemOpener(ItemOpener f) { itemOpener_ = std::move(f); }
    // What plugin screens made from now on show `items` blocks with (see PluginScreen); `items`
    // outlives them.
    void setItemUi(ItemSource* items, ItemTap tap) {
        items_ = items;
        itemTap_ = std::move(tap);
    }
    // Opens a plugin's screen over what shows (the launcher, an entry point): returns its instance.
    // Plugins not running (or no host): the screen shows "<Plugin> can't show this screen right now".
    uint64_t openScreen(App&, const std::string& pluginId, const std::string& screenId, nlohmann::json params = nullptr);
    // Home's body (Settings → Home, see app/home_tree.h): whether the plugin may show it now (plugins
    // run; it is on, not stopped, loads; once loaded, it has that screen with `home`), then the
    // screen opened without pushing anything (TreeUse::HomeBody; its trees taken as any screen's).
    // An instance of 0: nothing opened (plugins don't run).
    struct Opened {
        uint64_t instance = 0;
        std::string pluginName, title;
        std::string package;  // the plugin's folder (its images)
    };
    bool homeAvailable(const std::string& pluginId, const std::string& screenId);
    Opened openHome(const std::string& pluginId, const std::string& screenId);
    // Renders an open screen again (Home shown, a sync): to the host it belongs to; nothing for a
    // screen of a host gone with rebind() (it has its ScreenError waiting) or when plugins don't run.
    void rerender(uint64_t instance) override;
    // ScreenLink (PluginScreen).
    void send(uint64_t instance, nlohmann::json event) override;
    void left(uint64_t instance) override;
    bool take(uint64_t instance, PluginHostEvent& out) override;
    bool takeItemOpened(uint64_t instance) override { return itemOpened_.erase(instance) > 0; }
    // PluginEntries. attach() first: the app the launcher pushes screens on.
    void attach(App& app) { app_ = &app; }
    std::vector<PluginAction> actions(const std::string& kind) override;
    std::vector<PluginLaunch> mainScreens() override;
    std::vector<PluginLaunch> homeScreens() override;
    bool anyBadge() override;
    uint64_t version() override;
    uint64_t run(const PluginAction&, const std::string& kind, nlohmann::ordered_json target, nlohmann::json again = nullptr) override;
    std::optional<ActionOutcome> take(uint64_t run) override;
    void stop(uint64_t run) override;
    void openScreen(const std::string& pluginId, const std::string& screenId) override;

    static constexpr int64_t kToastGapMs = 3000;  // a toast's time on screen

private:
    PluginHost* host_;
    std::set<uint64_t> open_;
    std::deque<std::string> turnedOff_;  // toasts not shown yet
    int64_t lastToastMs_ = -kToastGapMs;
    std::function<void(uint64_t)> backgroundDone_;
    PreviewOpener previewOpener_;
    void openPreview(App&, const PluginHostEvent&);
    ItemOpener itemOpener_;
    ItemSource* items_ = nullptr;
    ItemTap itemTap_;
    void openItem(App&, const PluginHostEvent&);
    std::set<uint64_t> itemOpened_;  // screens whose zk.library.open opened an item (see takeItemOpened)
    bool running_ = true;
    std::function<bool()> refusing_;
    bool refusing() const { return refusing_ && refusing_(); }
    std::map<uint64_t, std::string> screens_;                // open plugin screens: instance → plugin name
    std::map<uint64_t, std::deque<PluginHostEvent>> mail_;   // what waits for them
    void pushScreen(App&, uint64_t instance, const std::string& pluginId, const std::string& pluginName, const std::string& title);
    App* app_ = nullptr;
    // The entries version at the last wake(): sleeping plugins are woken again only after something
    // changed (one turned on, one failed to load), so a plugin that can't load isn't asked forever.
    std::optional<uint64_t> wokenAt_;
    uint64_t rebinds_ = 0;
    std::map<uint64_t, std::string> runs_;            // actions running: run → plugin name
    std::map<uint64_t, ActionOutcome> outcomes_;      // come, not taken yet
    std::vector<PluginStatus> running();              // enabled, loaded plugins (wakes sleeping ones)
};

}  // namespace zk
