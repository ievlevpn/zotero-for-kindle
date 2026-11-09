#pragma once
// What a binding (a part of `zk`) works with: the plugin it serves, that plugin's JavaScript world,
// and the host's services — permissions, errors, host work that settles a promise later, and the
// user's confirmation. Everything here runs on the plugin thread unless it says otherwise.
//
// Adding a binding (Tasks 6, 7, 10, 13–16):
//  1. Write `void installStorage(PluginCtx&)` in its own bindings_*.cpp, adding members to `p.zk`
//     with defineFunction(); declare it below next to installCore().
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
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "plugins/host.h"
#include "plugins/js.h"
#include "plugins/manifest.h"
#include "plugins/plugin_log.h"

namespace zk::plugins {

struct Inbox;
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
};

// The plugin a native function was called for.
PluginCtx& ctxOf(JSContext*);

// obj[name] = a native function.
void defineFunction(JSContext*, JSValueConst obj, const char* name, JSCFunction*, int length);

// ---- Permissions and errors ---------------------------------------------------------------

// A permission as written in plugin.json ("library:read"; "net:" for Net, whose host varies).
std::string permissionName(Permission);
// True if the plugin declared `p`. Otherwise throws zk.PermissionError
// `<detail> needs "<permission>" in plugin.json` and returns false (return JS_EXCEPTION then).
bool requirePermission(PluginCtx&, Permission, const std::string& detail);
// Throws zk.PermissionError for a permission spelled out (e.g. "net:api.deepl.com"); returns
// JS_EXCEPTION.
JSValue throwPermissionError(PluginCtx&, const std::string& permission, const std::string& detail);
// A new zk.LimitError; limit is "time", "memory", "stack", "responseSize" or "responseTime".
// JS_EXCEPTION (pending) if it couldn't be made.
JSValue newLimitError(PluginCtx&, const std::string& limit, const std::string& message);

// ---- The running call ---------------------------------------------------------------------

struct CallInfo {
    uint64_t id = 0;  // 0: no call is running
    bool userCaused = false;
    bool background = false;
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

// ---- The user -----------------------------------------------------------------------------

// Posts `confirm` (kind Confirm; plugin id, name and request id are filled in) and pauses the
// running call's budget until the answer. `answered` runs on the plugin thread, outside any JS:
// settle a Work from it. Dropped (never called) if the call is stopped or the plugin unloaded.
uint64_t askUser(PluginCtx&, PluginHostEvent confirm, std::function<void(bool yes)> answered);
// Any other event for the screen (plugin id and name filled in).
void post(PluginCtx&, PluginHostEvent);

// ---- Host services the core bindings are built on -------------------------------------------

// A line in the plugin's log (and the log sink).
void log(PluginCtx&, LogLevel, const std::string& text);
// setTimeout/clearTimeout: a timer belongs to the running call (dropped if that call is stopped;
// run in its budget while it is running, else as a call of its own with the event budget).
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

}  // namespace zk::plugins
