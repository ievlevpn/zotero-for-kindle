#pragma once
// The plugin catalog in the app (part 3): one operation at a time on a worker thread, its state
// polled by the screens (as the Updater's). Network and file work never runs on the screen thread
// or the plugin thread; installing, updating and removing work in safe mode and with OFF.
//
//   check(force)   the registry and the installed plugins' releases (runCheck). Not forced, it is
//                  skipped while the copy is less than an hour old (Get plugins opening).
//   install(id)    a plugin that isn't installed: download, verify, put in place (off).
//   update(id)     an installed one: staged; applied at once when it adds no permissions, else
//                  awaitingYes holds it until applyStaged (Update) or discardStaged (Not now).
//   rollBack(id)   swaps it with the kept previous version.
//   remove(id)     after the confirmation: its files, kept version, storage and state.
// A running plugin is stopped first (PluginHost::hold) and rescanned after (release). Revoked ids
// are handed to the host (setWithdrawn) whenever the registry changes.

#include "catalog/check.h"
#include "catalog/install.h"
#include "net/http.h"
#include "update/download.h"
#include "update/version.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace zk {

class PluginHost;

struct CatalogConfig {
    std::string pluginsDir;
    std::function<std::unique_ptr<Http>()> makeHttp;
    std::string feed;                      // the registry feed (catalog::kRegistryFeed, or ZK_PLUGIN_REGISTRY)
    std::array<uint8_t, 32> registryKey{};  // the app's release key
    std::function<int64_t()> wallMs;
    Version app;
    DownloadHooks hooks;  // freeSpace, probe (tests)
    std::function<PluginHost*()> host;  // the host of the moment (null while an app update installs)
};

class CatalogService {
public:
    static constexpr int64_t kRegistryFreshMs = 3600 * 1000LL;  // Get plugins re-checks an older copy

    enum class Op { None, Load, Check, LookUp, Install, Update, Apply, Discard, RollBack, Remove };
    struct State {
        catalog::Cache cache;
        std::string busyId;    // the plugin an operation is working on ("" for a check)
        std::string busyText;  // "Downloading…", "Checking…", …
        std::string error;     // the last operation's failure, in a plain sentence ("" when it worked)
        std::optional<catalog::Staged> awaitingYes;  // an update that adds permissions, staged
        bool checking = false;
        Op busy = Op::None;
        // The last operation that ended: what, on which id, whether it worked.
        Op doneOp = Op::None;
        std::string doneId;
        bool doneOk = false;
        std::map<std::string, std::string> kept;  // id → the version kept in .previous/
        // Removes asked for and how each ended: waiting (queued or running), done, or failed (why).
        std::set<std::string> removing, removed;
        std::map<std::string, std::string> removeFailed;
        int serial = 0;  // changes with every change (screens compare it)
    };

    explicit CatalogService(CatalogConfig);
    ~CatalogService();
    CatalogService(const CatalogService&) = delete;
    CatalogService& operator=(const CatalogService&) = delete;

    // On the UI thread: start work, poll results; never blocks. A call while busy is ignored.
    void start();  // reads the cache (the worker)
    void check(bool force);
    void lookUp(std::string id);  // a catalog plugin's latest release, for its page (at most every 6 h)
    void install(std::string id);
    void update(std::string id);
    void updateAll();  // each waiting update that adds no permissions; the others stay waiting
    void applyStaged(std::string id);
    void discardStaged(std::string id);
    void rollBack(std::string id);
    // After the confirmation. Never dropped: queued while another operation runs. Its outcome shows
    // in State::removed / removeFailed.
    void remove(std::string id);
    void cancel();  // the operation in progress (a download), e.g. before wait() on the UI thread

    const State& state() const { return shown_; }
    bool poll();  // true when changed
    int updatesWaiting() const { return waiting_; }
    // The newer version for an installed plugin (its version as installed), if any.
    std::optional<catalog::Latest> updateFor(const std::string& id, const std::string& installedVersion) const;
    bool busy() const { return shown_.busy != Op::None; }
    void wait();  // until the current operation ends (tests; before the host goes)

private:
    void run(Op op, std::string id, std::string text, std::function<bool(std::string& error)> work);
    void set(const std::function<void(State&)>& change);
    bool swap(const std::string& id, PluginHost* host, const std::function<bool(UserError*)>& files, std::string& error);
    void refresh(State& s);  // kept versions, cache (worker)
    std::vector<std::string> installedIds(PluginHost*) const;
    void recount();
    void startRemove(const std::string& id);
    void forgetState(PluginHost*, const std::string& id);  // worker
    std::vector<std::string> queuedRemoves_;

    CatalogConfig config_;
    mutable std::mutex mutex_;
    State status_;  // the worker's
    State shown_;   // the UI thread's copy
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    int waiting_ = 0;
    uint64_t hostVersion_ = ~0ull;
    PluginHost* lastHost_ = nullptr;
};

}  // namespace zk
