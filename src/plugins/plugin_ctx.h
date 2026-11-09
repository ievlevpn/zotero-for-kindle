#pragma once
// What a binding (a part of `zk`) works with: the plugin it serves, that plugin's JavaScript world,
// and the host's services — permissions, errors, host work that settles a promise later, and the
// user's confirmation. Everything here runs on the plugin thread unless it says otherwise.
//
// Adding a binding (Tasks 6, 7, 10, 13–16; bindings_storage.cpp and bindings_fetch.cpp are examples):
//  1. Write `void installStorage(PluginCtx&)` in its own bindings_*.cpp, adding members to `p.zk`
//     with defineFunction<native>() — the one way to define a native: it catches every C++
//     exception at the boundary (QuickJS's C frames can't unwind one). Declare it below next to
//     installCore().
//  2. Call it from installBindings() in host.cpp (the list of built-in installers, in order).
//  3. Native functions find their plugin with ctxOf(cx). Check permissions first with
//     requirePermission(); it throws the plugin-facing PermissionError. Inside a native, call
//     JavaScript with JS_Call, never js::Context::call/evalModule/runJobs: those start a new
//     operation, which re-anchors the engine's stack check deeper (catch-and-recurse code could
//     then overrun the thread's stack) and records exceptions as the running call's failure.
//  4. Async work: startWork() gives the promise to return and a Work handle. Do the work anywhere
//     (a worker thread, or right away) and settle it with Work::finish/resolve/reject from any
//     thread; the promise settles on the plugin thread, in the budget of the call that started
//     it. If that call is stopped by a limit, the Work is cancelled (cancelled() turns true, its
//     cancelFlag() can be handed to HttpRequest::cancel) and the promise rejects with
//     zk.LimitError at once; a later finish() is ignored.
//  5. Questions to the user: askUser() posts a Confirm event and pauses the asking call's budget
//     until PluginHost::answer() (Task 13).

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "plugins/host.h"
#include "plugins/js.h"
#include "plugins/manifest.h"
#include "plugins/plugin_log.h"

namespace zk {
class Library;
class PluginState;  // state.h
}

namespace zk::plugins {

struct Inbox;
class PluginNet;   // plugin_http.h
struct PluginRun;  // the host's bookkeeping for one loaded plugin (host.cpp)

struct PluginCtx {
    PluginCtx(PluginHost::Impl& host, PluginManifest manifest, std::string dir, size_t memoryLimit);
    ~PluginCtx();
    PluginCtx(const PluginCtx&) = delete;
    PluginCtx& operator=(const PluginCtx&) = delete;

    PluginHost::Impl& host;
    const PluginManifest manifest;
    const std::string dir;      // <pluginsDir>/<id>
    const std::string dataDir;  // <dir>/data (zk.plugin.dataDir; zk.files works inside it)
    nlohmann::json settings = nlohmann::json::object();  // settings.json, or {}
    // Destruction order matters: every Value dies before its Context, the Context before the
    // Runtime (members are destroyed bottom-up).
    js::Runtime rt;
    js::Context cx;
    js::Value zk;  // the plugin's global `zk`; installers add their members to it
    js::Value makePermissionError, makeLimitError;  // the error classes' factories (installCore)
    std::unique_ptr<PluginRun> run;
    std::shared_ptr<void> storage;  // bindings_storage.cpp's state (no JavaScript values in it)
    std::shared_ptr<void> zotero;   // bindings_zotero.cpp's state (no JavaScript values in it)
};

// The plugin a native function was called for.
PluginCtx& ctxOf(JSContext*);

// What a native's C++ exception becomes in JS: std::bad_alloc the memory limit (an out-of-memory
// error the host counts as a memory stop), anything else an Error("The app failed doing this: …").
JSValue nativeFailed(JSContext*, const char* what, bool outOfMemory) noexcept;

// Every native runs inside this: no C++ exception ever crosses into QuickJS.
template <JSCFunction* F>
JSValue guardedNative(JSContext* c, JSValueConst self, int argc, JSValueConst* argv) noexcept {
    try {
        return F(c, self, argc, argv);
    } catch (const std::bad_alloc&) {
        return nativeFailed(c, "out of memory", true);
    } catch (const std::exception& e) {
        return nativeFailed(c, e.what(), false);
    } catch (...) {
        return nativeFailed(c, "an unknown error", false);
    }
}

// obj[name] = native F, behind guardedNative.
template <JSCFunction* F>
void defineFunction(JSContext* c, JSValueConst obj, const char* name, int length) {
    JS_SetPropertyStr(c, obj, name, JS_NewCFunction(c, guardedNative<F>, name, length));
}

// ---- Permissions and errors ---------------------------------------------------------------

// A permission as written in plugin.json ("library:read"; "net:" for Net, whose host varies).
std::string permissionName(Permission);
// True if the plugin declared `p`. Otherwise throws zk.PermissionError
// `<detail> needs "<permission>" in plugin.json` and returns false (return JS_EXCEPTION then).
bool requirePermission(PluginCtx&, Permission, const std::string& detail);
// Throws zk.PermissionError for a permission spelled out (e.g. "net:api.deepl.com"); returns
// JS_EXCEPTION.
JSValue throwPermissionError(PluginCtx&, const std::string& permission, const std::string& detail);
// A new zk.LimitError; limit is "time", "memory", "stack", "responseSize", "responseTime" or
// "fetches".
// JS_EXCEPTION (pending) if it couldn't be made.
JSValue newLimitError(PluginCtx&, const std::string& limit, const std::string& message);

// ---- The running call ---------------------------------------------------------------------

struct CallInfo {
    uint64_t id = 0;  // 0: no call is running
    bool userCaused = false;
    bool background = false;
    bool cleanup = false;  // the code of a stopped call, running its last rejections
};
CallInfo currentCall(PluginCtx&);

// ---- Host work ----------------------------------------------------------------------------

// Runs on the plugin thread: returns the value to resolve with (ownership passes), or
// JS_EXCEPTION with an exception pending to reject with it.
using Settle = std::function<JSValue(JSContext*)>;

class Work {
public:
    Work(std::weak_ptr<Inbox> inbox, uint64_t ctxSerial, uint64_t id);
    // Any thread. True once the work's call was stopped or its plugin unloaded: stop working.
    bool cancelled() const { return cancel_->load(); }
    std::shared_ptr<std::atomic<bool>> cancelFlag() const { return cancel_; }
    // Any thread, at most once (later calls are ignored): settles the promise on the plugin thread.
    void finish(Settle);
    void resolve(nlohmann::json value);
    void reject(std::string message);  // with an Error(message)
    uint64_t id() const { return id_; }

private:
    std::weak_ptr<Inbox> inbox_;
    uint64_t ctxSerial_, id_;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> finished_{false};
};

// Starts host work owned by the running call: *promise receives the promise to hand to JS.
std::shared_ptr<Work> startWork(PluginCtx&, JSValue* promise);

// The host's network workers (zk.fetch), made at the first use; nullptr if the host has no makeHttp.
PluginNet* netOf(PluginCtx&);

// The host's settings (database path, engine environment, plugins folder).
const PluginHostDeps& depsOf(PluginCtx&);
// What the user and the app remember about the plugins (on/off, failures, badge, last background run).
PluginState& pluginStateOf(PluginCtx&);
// One slot per host for state shared by its plugins (all on the plugin thread): zk.library's
// database connection and identifier index.
std::shared_ptr<void>& librarySlot(PluginCtx&);
// The plugin thread's Library on that connection (bindings_library.cpp), opened at the first use;
// nullptr with the reason in *why if there is no library database.
Library* sharedLibrary(PluginCtx&, std::string* why);

// ---- The user -----------------------------------------------------------------------------

// Posts `confirm` (kind Confirm; plugin id, name and request id are filled in) and pauses the
// running call's budget until the answer. `answered` runs on the plugin thread, outside any JS:
// settle a Work from it. Dropped (never called) if the call is stopped or the plugin unloaded.
// Returns 0 (nothing posted, `answered` never called) when asked from a stopped call's code.
uint64_t askUser(PluginCtx&, PluginHostEvent confirm, std::function<void(bool yes)> answered);
// Any other event for the screen (plugin id and name filled in).
void post(PluginCtx&, PluginHostEvent);

// UTF-8 with every invalid byte replaced by U+FFFD.
std::string sanitisedUtf8(const char* s, size_t n);

// ---- Host services the core bindings are built on -------------------------------------------

// A line in the plugin's log (and the log sink).
void log(PluginCtx&, LogLevel, const std::string& text);
// setTimeout/clearTimeout: a timer belongs to the running call (dropped if that call is stopped;
// run in its budget while it is running, else as a call of its own with the event budget). A
// timer set by a stopped call's cleanup code belongs to no call: it runs as a call of its own.
uint64_t addTimer(PluginCtx&, js::Value callback, int64_t delayMs);
void clearTimer(PluginCtx&, uint64_t id);
// zk.on / zk.background registrations.
void addHandler(PluginCtx&, const std::string& event, js::Value handler);
void setBackgroundTask(PluginCtx&, js::Value task);

// ---- Installers ---------------------------------------------------------------------------

// The standard globals: URL, URLSearchParams, TextEncoder, TextDecoder. False on an internal
// error (logged).
bool installStandardGlobals(js::Context&);
// zk.plugin, zk.settings, zk.on, zk.background, zk.PermissionError, zk.LimitError, console,
// setTimeout/clearTimeout, and the standard globals. Throws std::runtime_error on an internal
// error (a bug in the preludes, or no memory).
void installCore(PluginCtx&);
// zk.storage and zk.files (bindings_storage.cpp). Throws std::runtime_error on an internal error.
void installStorage(PluginCtx&);
// zk.fetch (bindings_fetch.cpp; the guard and workers are plugin_http.h). Throws std::runtime_error
// on an internal error.
void installFetch(PluginCtx&);
// zk.library (bindings_library.cpp). Throws std::runtime_error on an internal error.
void installLibrary(PluginCtx&);
// zk.zotero (bindings_zotero.cpp). Throws std::runtime_error on an internal error.
void installZotero(PluginCtx&);
// Empties <pluginDir>/data/ and storage.db. `loaded`, if the plugin is loaded: its queued writes
// are dropped and its storage closed (it reopens, empty, at the next use).
void resetPluginData(const std::string& pluginDir, PluginCtx* loaded);

}  // namespace zk::plugins
