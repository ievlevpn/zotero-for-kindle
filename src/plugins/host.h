#pragma once
// The plugin host: one plugin thread running every enabled plugin, each in its own QuickJS runtime
// and context. The screen thread never waits on JavaScript: it queues commands (dispatch,
// runBackground, answer, reload, setEnabled) and polls PluginHostEvents, like sync and render
// events. See plugin_ctx.h for how bindings (zk.*) plug in.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "docs/document.h"
#include "plugins/events.h"
#include "plugins/manifest.h"
#include "plugins/ui_tree.h"
#include "update/version.h"

namespace zk {

class Http;

namespace plugins {
struct PluginCtx;
// The plugin thread and its workers (fetch, full text) call this first: they run below the screen
// and sync threads (nice 10 on Linux; 10 below the default scheduling priority on macOS), so reading never stutters
// for a plugin.
void lowerThreadPriority();
}

struct PluginHostDeps {
    std::string pluginsDir;
    // The library database. Plugins open their own connection (on the plugin thread), like the sync worker.
    std::string dbPath;
    // Where the engines find their data (zk.library.fullText opens its own engine).
    EngineEnv engineEnv;
    // Names the plugin whose code is running (written before a plugin's code runs, removed when
    // the plugin thread goes idle). Empty: no marker.
    std::string crashMarker;
    Version appVersion;
    std::function<std::unique_ptr<Http>()> makeHttp;
    std::string webdavHost;  // the configured WebDAV server (host or URL); later: setWebdavHost
    std::function<int64_t()> clock;  // wall clock, ms since the epoch (log times); default: system
    // The desktop developer loop (--plugins-dir): start() loads every enabled plugin at once, and
    // reload(id) loads an enabled plugin that wasn't loaded yet. Off: plugins load lazily, at their
    // first event (the Kindle).
    bool loadEagerly = false;
    size_t memoryPerPlugin = 32 << 20;
    // What all plugins together may use (actual use, shared): an allocation that would pass it
    // fails, stopping the call that made it.
    size_t memoryTotal = 96u << 20;
    int handlerBudgetMs = 5000, backgroundBudgetMs = 30000;
    // A screen's onEvent (and an action's run): user-caused, at least 30 s. A screen's render: 1 s.
    int actionBudgetMs = 30000, renderBudgetMs = 1000;
    // Quiet run time: outside user-caused calls and the background run, a plugin's JavaScript may
    // run at most quietRunMs in any quietWindowMs; a timer or continuation whose call has ended
    // waits (not failed) until the window allows it.
    int quietRunMs = 2000, quietWindowMs = 60000;
    // Free bytes on the partition holding a path (statvfs; tests inject their own): plugin writes
    // (zk.files, zk.storage) are refused when it is under 50 MB.
    std::function<uint64_t(const std::string& path)> freeSpace;
    // Each line written to a plugin's log is also handed here (on the plugin thread): the desktop
    // build mirrors it to stderr when asked to.
    std::function<void(const std::string& pluginId, const std::string& line)> logSink;
    // More bindings installed into every plugin's `zk` after the built-in ones (tests use this to
    // give plugins host work they control).
    std::vector<std::function<void(plugins::PluginCtx&)>> extraBindings;
};

// Confirm: the app's own confirmation of a removal (zk.zotero): `text` the question, `lines` what
// goes when there are several, `action` the yes button ("Move to trash"); answer it with
// PluginHost::answer(requestId). Withdrawn: that request no longer waits (its call was stopped, or
// the plugin turned off or reloaded): take the question away without answering.
// BackgroundDone: the tasks of a runBackground() have all ended (also when none was due): the
// screen thread's keep-awake hold may end; `requestId` is the round it answers. Badge: `text` is the plugin's new badge ("" cleared).
//
// Screens (zk.screen; `instance` names one open screen, see PluginHost::openScreen):
// ScreenTree: `tree` is what render returned, checked; `requestId` counts the events of the screen
// sent with screenEvent() that were handled before it (an event is answered by the first tree
// whose requestId reaches its number, 1 for the first). ScreenOpen: the plugin opened `screenId`
// (title in `text`) over `parent` with zk.ui.open (parent 0: from an action's run); `value` the
// params (null: none). ScreenClose: the plugin closed the screen (zk.ui.close); `value` the result.
// ScreenError: the screen can't be shown (render failed, an invalid tree, the plugin turned off or
// restarted): `text` "<Plugin> can't show this screen right now"; nothing more comes for it.
// ScreenActionError: its onEvent failed: `text` "<Plugin> couldn't do that: <message>"; a tree follows.
struct PluginHostEvent {
    enum class Kind {
        Confirm, Badge, Disabled, ChangesQueued, Log, Withdrawn, BackgroundDone,
        ScreenTree, ScreenOpen, ScreenClose, ScreenError, ScreenActionError
    } kind;
    std::string pluginId, pluginName, text;
    std::vector<std::string> lines;
    uint64_t requestId = 0;
    std::string action;
    uint64_t instance = 0, parent = 0;
    std::string screenId;
    nlohmann::json value;
    std::shared_ptr<const Block> tree;
};

// What a plugin registered with zk.selectionAction / zk.itemAction / zk.screen.
// kind: "selectionAction", "itemAction" or "screen"; `main`: a screen listed in the launcher.
struct PluginEntry {
    std::string kind, id, title;
    bool main = false;
};

struct PluginStatus {
    PluginFolder folder;
    bool enabled = false, loaded = false;
    // everEnabled: turned on at some time (a copied plugin isn't until its permissions were shown).
    // stopped: the app turned it off (three failures in a row, or the app stopped while it ran).
    bool everEnabled = false, stopped = false;
    int failures = 0;      // in a row, not cleared by a success yet
    std::string problem;   // why it couldn't load this time ("" if none; also the last of errors)
    std::string badge;
    std::vector<std::string> errors;  // newest last: past failures, then a current problem
    std::vector<PluginEntry> entries;  // registered by the loaded plugin, in order; empty when not loaded
};

class PluginHost {
public:
    // Scans the plugins folder (nothing runs yet). If the crash marker names a plugin, the app
    // stopped while it was running: it is turned off, with a note, and the marker removed.
    explicit PluginHost(PluginHostDeps);
    ~PluginHost();  // stop()
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Starts the plugin thread (with deps.loadEagerly, loading every enabled plugin). Plugins load lazily: each enabled plugin when the first event or
    // background run is due. Commands queued before start() wait for it.
    void start();
    // Interrupts whatever plugin code is running and joins the thread; returns within 2 s. Calls
    // cut short by stop() are not counted as failures.
    void stop();
    // Unloads and rescans; plugins that were loaded are loaded again at once.
    void reload();
    void reload(const std::string& id);
    void dispatch(PluginEvent);
    // Runs the background task of each plugin that has one and hasn't run, failed or gone over its
    // budget in the last hour (PluginState::lastBackgroundMs, persisted). Calls cut short (stop(),
    // cancelBackground(), the plugin turned off) don't count: that plugin is due again. One
    // BackgroundDone event follows when the tasks have ended; its requestId is `round` (the caller's
    // number for this run, so a late Done of an earlier round can be told apart). A task whose
    // zk.fetch died of a lost connection isn't counted either.
    void runBackground(uint64_t round = 0);
    // Is some enabled plugin's background task due (see above)? Cheap and any thread: loads nothing.
    // A plugin not loaded yet whose manifest has the background permission counts as possibly due.
    bool backgroundDue() const;
    // Some installed plugin that can load is turned on (cheap; any thread): with none, the app
    // builds no events for plugins.
    bool anyEnabled() const;
    // The Kindle is going to sleep: stops background tasks still running. Not failures.
    void cancelBackground();
    // Empties the plugin's data/ folder and its storage (Settings → Plugins → Reset its data). Works
    // whether or not the plugin is loaded; if it is, its queued storage writes are dropped.
    void resetData(const std::string& id);
    // Opens a plugin's screen (the launcher, a test): returns its instance number at once; the
    // plugin gets an `open` event (if it has onEvent), then render runs and a ScreenTree (or a
    // ScreenError) follows. `params` (null: none) are given to render and onEvent.
    uint64_t openScreen(const std::string& pluginId, const std::string& screenId, nlohmann::json params = nullptr);
    // What the user did on an open screen ({kind:"tap", id}, …: zk.d.ts's zk.ui.Event). Events are
    // handled one at a time, in order; each is answered by a ScreenTree (see PluginHostEvent).
    void screenEvent(uint64_t instance, nlohmann::json event);
    // The user left the screen (Back, or it went with its tab): it is closed now; the plugin gets
    // a `back` event, and the screen below a `result` event.
    void closeScreen(uint64_t instance);
    // The user's answer to a Confirm event.
    void answer(uint64_t requestId, bool yes);
    bool poll(PluginHostEvent&);
    std::vector<PluginStatus> status() const;
    // Turning a plugin on clears its failures; turning it off unloads it. Ignored for an id that
    // isn't a listed folder, and turning on for a folder with a problem (broken, incompatible).
    void setEnabled(const std::string& id, bool);
    // Settings → Plugins → Remove: off at once and no longer listed by status(); then, on the plugin
    // thread, unloaded and its folder deleted (data, settings and all). What it remembered goes
    // too, so a copy put back is new again. Its changes for Zotero stay (pending ones are sent).
    void remove(const std::string& id);
    // The WebDAV server changed (host or URL; "" for none): zk.fetch refuses the new one from the
    // next request on. Any thread.
    void setWebdavHost(std::string);

    // Blocks until no command is queued and no call is running or awaiting (timers may be
    // pending), or `timeoutMs` passes. True if idle. For tests and the background keep-awake hold.
    bool waitIdle(int timeoutMs);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace zk
