#include "app/catalog_service.h"

#include "core/log.h"
#include "plugins/host.h"
#include "plugins/state.h"

#include <algorithm>
#include <filesystem>

namespace zk {

namespace fs = std::filesystem;
using namespace catalog;

namespace {

std::map<std::string, std::string> keptVersions(const std::string& pluginsDir) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(fs::path(pluginsDir) / ".previous", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string id = it->path().filename().string();
        if (auto v = previousVersion(pluginsDir, id)) out[id] = *v;
    }
    return out;
}

std::optional<std::string> installedVersion(PluginHost* host, const std::string& id) {
    if (!host) return std::nullopt;
    for (const auto& s : host->status())
        if (s.folder.dir == id && s.folder.manifest) return s.folder.manifest->version;
    return std::nullopt;
}

bool revoked(const Cache& c, const std::string& id) {
    if (!c.registry) return false;
    for (const auto& r : c.registry->revoked)
        if (r.id == id) return true;
    return false;
}

}  // namespace

CatalogService::CatalogService(CatalogConfig c) : config_(std::move(c)) {
    if (!config_.wallMs) config_.wallMs = [] { return int64_t{0}; };
}

CatalogService::~CatalogService() {
    *cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void CatalogService::set(const std::function<void(State&)>& change) {
    std::lock_guard<std::mutex> lock(mutex_);
    change(status_);
    ++status_.serial;
}

void CatalogService::refresh(State& s) {
    s.cache = loadCache(config_.pluginsDir);
    s.kept = keptVersions(config_.pluginsDir);
}

void CatalogService::run(Op op, std::string id, std::string text, std::function<bool(std::string& error)> work) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.busy != Op::None) return;
    }
    if (worker_.joinable()) worker_.join();  // the previous operation has ended
    cancel_ = std::make_shared<std::atomic<bool>>(false);  // this operation's own
    set([&](State& s) {
        s.busy = op;
        s.busyId = id;
        s.busyText = text;
        s.error.clear();
        s.notice.clear();
        s.checking = op == Op::Check;
    });
    {  // the screens see it at once (and a second call before the next poll is ignored)
        std::lock_guard<std::mutex> lock(mutex_);
        shown_ = status_;
    }
    worker_ = std::thread([this, op, id, work = std::move(work)] {
        std::string error;
        const bool ok = work(error);
        State fresh;
        refresh(fresh);
        set([&](State& s) {
            s.cache = std::move(fresh.cache);
            s.kept = std::move(fresh.kept);
            s.busy = Op::None;
            s.busyId.clear();
            s.busyText.clear();
            s.checking = false;
            s.error = ok ? "" : error;
            s.doneOp = op;
            s.doneId = id;
            s.doneOk = ok;
            if (op == Op::Install && ok) s.installed.insert(id);
            if (op == Op::Install && !ok) s.installFailed[id] = error;
        });
    });
}

std::vector<std::string> CatalogService::installedIds(PluginHost* host) const {
    std::vector<std::string> out;
    if (host)
        for (const auto& s : host->status()) out.push_back(s.folder.dir);
    return out;
}

// Stops the plugin, swaps its files, puts it back (a failure repaired first, as install.h asks).
bool CatalogService::swap(const std::string& id, PluginHost* host, const std::function<bool(UserError*)>& files,
                          std::string& error) {
    if (host && !host->hold(id)) {
        host->release(id);
        error = "Couldn't stop the plugin. Try again.";
        return false;
    }
    UserError e;
    const bool ok = files(&e);
    if (!ok) {
        repairOne(config_.pluginsDir, id);
        error = e.message;
    }
    if (host) host->release(id);
    return ok;
}

void CatalogService::start() {
    run(Op::Load, "", "", [](std::string&) { return true; });
}

void CatalogService::check(bool force, std::string pageId) {
    if (!force && shown_.cache.registry) {
        const int64_t age = config_.wallMs() - shown_.cache.registryMs;
        if (age >= 0 && age < kRegistryFreshMs) {
            if (!pageId.empty()) lookUp(pageId, kPageCheckEveryMs);  // the registry is fresh: only its release
            return;
        }
    }
    const auto ids = installedIds(config_.host ? config_.host() : nullptr);
    run(Op::Check, "", "Checking…", [this, ids, force, pageId](std::string& error) {
        auto http = config_.makeHttp();
        const catalog::CheckResult r = runCheck(*http, {config_.feed, config_.registryKey, config_.wallMs}, config_.pluginsDir, ids,
                                       force, cancel_, pageId);
        log(LogLevel::Info, "catalog: checked: %s", r.status == catalog::CheckStatus::Ok ? "ok" : r.message.c_str());
        const bool none = r.status == catalog::CheckStatus::NoCatalog;  // calm: not an error
        set([&](State& s) { s.noCatalog = none; });
        error = r.status == catalog::CheckStatus::Offline ? "No Wi-Fi." : r.message;
        return r.status == catalog::CheckStatus::Ok || none;
    });
}

void CatalogService::lookUp(std::string id, int64_t everyMs) {
    const Entry* e = find(shown_.cache, id);
    if (!e) return;
    if (auto l = shown_.cache.latest.find(id); l != shown_.cache.latest.end()) {
        const int64_t age = config_.wallMs() - l->second.checkedMs;
        if (age >= 0 && age < everyMs) return;
    }
    run(Op::LookUp, id, "", [this, entry = *e](std::string& error) {
        auto http = config_.makeHttp();
        auto l = fetchLatest(*http, entry, config_.wallMs(), cancel_);
        if (!l) {
            error = "Couldn't read the plugin's latest release.";
            return false;
        }
        Cache c = loadCache(config_.pluginsDir);
        c.latest[entry.id] = *l;
        saveCache(config_.pluginsDir, c);
        return true;
    });
}

void CatalogService::startInstall(const std::string& id) {
    const Entry* e = find(shown_.cache, id);
    PluginHost* host = config_.host ? config_.host() : nullptr;
    if (!e || revoked(shown_.cache, id) || installedVersion(host, id)) return;
    std::optional<Latest> cached;
    if (auto l = shown_.cache.latest.find(id); l != shown_.cache.latest.end()) cached = l->second;
    run(Op::Install, id, "Downloading…", [this, entry = *e, cached, host](std::string& error) {
        auto http = config_.makeHttp();
        auto latest = fetchLatest(*http, entry, config_.wallMs(), cancel_);
        if (!latest) latest = cached;
        if (!latest) {
            error = "Couldn't read the plugin's latest release.";
            return false;
        }
        UserError err;
        if (!stage(*http, entry, *latest, config_.pluginsDir, config_.app, cancel_, &err, config_.hooks)) {
            error = err.message;
            return false;
        }
        // A fresh folder starts fresh: off, never turned on, so its page shows what it can do first
        // (state left from a copy deleted by hand mustn't turn it on).
        std::error_code ec;
        if (!fs::exists(fs::path(config_.pluginsDir) / entry.id, ec)) forgetState(host, entry.id);
        if (!swap(entry.id, host, [&](UserError* e) { return commit(config_.pluginsDir, entry.id, e); }, error))
            return false;
        Cache c = loadCache(config_.pluginsDir);
        c.latest[entry.id] = *latest;
        saveCache(config_.pluginsDir, c);
        return true;
    });
}

void CatalogService::startUpdate(const std::string& id) {
    PluginHost* host = config_.host ? config_.host() : nullptr;
    const Entry* e = find(shown_.cache, id);
    const auto version = installedVersion(host, id);
    if (!e || !version || revoked(shown_.cache, id)) return;
    const auto latest = catalog::updateFor(shown_.cache, id, *version);
    if (!latest) return;
    run(Op::Update, id, "Downloading…", [this, entry = *e, latest = *latest, host](std::string& error) {
        auto http = config_.makeHttp();
        UserError err;
        auto staged = stage(*http, entry, latest, config_.pluginsDir, config_.app, cancel_, &err, config_.hooks);
        if (!staged) {
            error = err.message;
            return false;
        }
        if (!staged->added.empty()) {  // never applied without the yes
            set([&](State& s) {
                s.needsYes[entry.id] = latest.version.str();
                s.awaitingYes = std::move(*staged);
            });
            return true;
        }
        return swap(entry.id, host, [&](UserError* e) { return commit(config_.pluginsDir, entry.id, e); }, error);
    });
}

void CatalogService::updateAll() {
    PluginHost* host = config_.host ? config_.host() : nullptr;
    if (!host) return;
    std::vector<std::pair<Entry, Latest>> todo;
    const std::string asking = shown_.awaitingYes ? shown_.awaitingYes->manifest.id : "";
    for (const auto& s : host->status()) {
        if (!s.folder.manifest || revoked(shown_.cache, s.folder.dir) || s.folder.dir == asking) continue;
        const Entry* e = find(shown_.cache, s.folder.dir);
        auto l = catalog::updateFor(shown_.cache, s.folder.dir, s.folder.manifest->version);
        const auto yes = shown_.needsYes.find(s.folder.dir);
        if (e && l && !(yes != shown_.needsYes.end() && yes->second == l->version.str())) todo.emplace_back(*e, *l);
    }
    if (todo.empty()) return;
    run(Op::Update, "", "Downloading…", [this, todo, host](std::string& error) {
        auto http = config_.makeHttp();
        bool all = true;
        for (const auto& [entry, latest] : todo) {
            if (*cancel_) break;
            UserError err;
            auto staged = stage(*http, entry, latest, config_.pluginsDir, config_.app, cancel_, &err, config_.hooks);
            std::string why;
            if (!staged) {
                why = err.message;
            } else if (!staged->added.empty()) {  // it asks for more: its own page asks first
                std::error_code ec;
                fs::remove_all(fs::path(config_.pluginsDir) / ".incoming" / entry.id, ec);
                fs::remove(fs::path(config_.pluginsDir) / ".incoming" / (entry.id + ".zip"), ec);
                set([&](State& s) { s.needsYes[entry.id] = latest.version.str(); });
                if (all) error = entry.name + " asks for more: open it to update.";
                all = false;
                continue;
            } else if (swap(entry.id, host, [&](UserError* e) { return commit(config_.pluginsDir, entry.id, e); }, why)) {
                continue;
            }
            if (all) error = entry.name + ": " + why;
            all = false;
        }
        return all;
    });
}

void CatalogService::applyStaged(std::string id) {
    if (busy() || !shown_.awaitingYes || shown_.awaitingYes->manifest.id != id) return;
    PluginHost* host = config_.host ? config_.host() : nullptr;
    set([](State& s) { s.awaitingYes.reset(); });
    shown_.awaitingYes.reset();
    run(Op::Apply, id, "Updating…", [this, id, host](std::string& error) {
        return swap(id, host, [&](UserError* e) { return commit(config_.pluginsDir, id, e); }, error);
    });
}

void CatalogService::discardStaged(std::string id) {
    if (busy() || !shown_.awaitingYes || shown_.awaitingYes->manifest.id != id) return;
    set([](State& s) { s.awaitingYes.reset(); });
    shown_.awaitingYes.reset();  // gone from the page at once
    run(Op::Discard, id, "", [this, id](std::string&) {
        std::error_code ec;
        const fs::path incoming = fs::path(config_.pluginsDir) / ".incoming";
        fs::remove_all(incoming / id, ec);
        fs::remove(incoming / (id + ".zip"), ec);
        if (fs::is_empty(incoming, ec)) fs::remove(incoming, ec);
        return true;
    });
}

void CatalogService::startRollBack(const std::string& id) {
    if (!shown_.kept.count(id)) return;
    PluginHost* host = config_.host ? config_.host() : nullptr;
    run(Op::RollBack, id, "Rolling back…", [this, id, host](std::string& error) {
        // The kept version may ask for what the user hasn't seen: then it is left off, and its page
        // shows what it can do before it is turned on again.
        const auto adds = rollBackAdds(config_.pluginsDir, id);
        if (!swap(id, host, [&](UserError* e) { return catalog::rollBack(config_.pluginsDir, id, e); }, error)) return false;
        if (!adds.empty()) {
            if (host) host->setEnabled(id, false);
            else PluginState(config_.pluginsDir + "/.state.json").setEnabled(id, false);
            set([](State& s) { s.notice = "It is off: this version can do more. Turn it on to allow it."; });
        }
        return true;
    });
}

void CatalogService::install(std::string id) { enqueue(Op::Install, id, "Downloading…"); }
void CatalogService::update(std::string id) { enqueue(Op::Update, id, "Downloading…"); }
void CatalogService::rollBack(std::string id) { enqueue(Op::RollBack, id, "Rolling back…"); }

// Install, Update, Roll back and Remove are never dropped: one asked while another operation runs
// waits for it (in order), its busy text showing at once.
void CatalogService::enqueue(Op op, const std::string& id, const std::string& text) {
    for (const auto& q : queue_)
        if (q.first == op && q.second == id) return;
    queue_.emplace_back(op, id);
    set([&](State& s) {
        s.queued[id] = text;
        if (op == Op::Install) {
            s.installed.erase(id);
            s.installFailed.erase(id);
        }
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shown_.queued = status_.queued;
    }
    poll();  // starts it now when nothing else runs
}

void CatalogService::forgetState(PluginHost* host, const std::string& id) {
    if (host) host->forgetState(id);
    else PluginState(config_.pluginsDir + "/.state.json").forget(id);  // no host (an app update installing)
}

void CatalogService::cancel() { *cancel_ = true; }

void CatalogService::remove(std::string id) {
    set([&](State& s) {
        s.removing.insert(id);
        s.removed.erase(id);
        s.removeFailed.erase(id);
    });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shown_.removing = status_.removing;
        shown_.removed = status_.removed;
        shown_.removeFailed = status_.removeFailed;
    }
    enqueue(Op::Remove, id, "Removing…");
}

void CatalogService::startRemove(const std::string& id) {
    PluginHost* host = config_.host ? config_.host() : nullptr;
    run(Op::Remove, id, "Removing…", [this, id, host](std::string& error) {
        const auto finish = [&](bool ok) {
            set([&](State& s) {
                s.removing.erase(id);
                if (ok) s.removed.insert(id);
                else s.removeFailed[id] = error.empty() ? "Couldn't remove the plugin." : error;
            });
            return ok;
        };
        if (host && !host->hold(id)) {
            host->release(id);
            error = "Couldn't stop the plugin. Try again.";
            return finish(false);
        }
        UserError e;
        const bool ok = removeFiles(config_.pluginsDir, id, &e);
        if (!ok) error = e.message;
        if (ok) {
            if (host) host->forget(id);  // its storage went with its folder; its state goes here
            else forgetState(nullptr, id);
        }
        if (host) host->release(id);
        return finish(ok);
    });
}

std::optional<Latest> CatalogService::updateFor(const std::string& id, const std::string& installedVersion) const {
    if (revoked(shown_.cache, id)) return std::nullopt;
    return catalog::updateFor(shown_.cache, id, installedVersion);
}

void CatalogService::recount() {
    PluginHost* host = config_.host ? config_.host() : nullptr;
    int n = 0, all = 0;
    std::map<std::string, std::string> withdrawn;
    if (host) {
        for (const auto& s : host->status()) {
            if (!s.folder.manifest) continue;
            if (auto l = updateFor(s.folder.dir, s.folder.manifest->version)) {
                ++n;
                const auto yes = shown_.needsYes.find(s.folder.dir);
                if (!(yes != shown_.needsYes.end() && yes->second == l->version.str())) ++all;
            }
            if (shown_.cache.registry)
                for (const auto& r : shown_.cache.registry->revoked)
                    if (r.id == s.folder.dir) withdrawn[r.id] = r.reason;
        }
        host->setWithdrawn(std::move(withdrawn));  // a no-op when unchanged
    }
    waiting_ = n;
    forAll_ = all;
}

bool CatalogService::poll() {
    while (!queue_.empty()) {  // what was asked while another operation ran
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shown_.busy = status_.busy;
            if (status_.serial != shown_.serial) shown_ = status_;  // starts from what is known now
        }
        if (busy()) break;
        const auto [op, id] = queue_.front();
        queue_.erase(queue_.begin());
        set([&, id = id](State& s) { s.queued.erase(id); });
        if (op == Op::Install) startInstall(id);
        else if (op == Op::Update) startUpdate(id);
        else if (op == Op::RollBack) startRollBack(id);
        else startRemove(id);
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.serial != shown_.serial) {
            shown_ = status_;
            changed = true;
        }
    }
    PluginHost* host = config_.host ? config_.host() : nullptr;
    const uint64_t v = host ? host->entriesVersion() : 0;
    if (changed || host != lastHost_ || v != hostVersion_) {
        lastHost_ = host;
        const int before = waiting_;
        recount();
        hostVersion_ = host ? host->entriesVersion() : 0;  // setWithdrawn may have moved it
        changed = changed || before != waiting_;
    }
    return changed;
}

void CatalogService::wait() {
    if (worker_.joinable()) worker_.join();
}

}  // namespace zk
