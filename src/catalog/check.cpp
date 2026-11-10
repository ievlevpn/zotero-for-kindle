#include "catalog/check.h"

#include "update/signature.h"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace zk::catalog {

const char* const kRegistryFeed = "https://api.github.com/repos/ievlevpn/zotero-for-kindle-plugins/releases/latest";

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

std::optional<std::string> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAtomic(const fs::path& p, const std::string& text) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + "." + std::to_string(::getpid()) + "." + std::to_string(counter++) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code rm;
        fs::remove(tmp, rm);
        return false;
    }
    return true;
}

HttpRequest get(const std::string& url, const std::shared_ptr<std::atomic<bool>>& cancel, size_t maxBytes) {
    HttpRequest req;
    req.url = url;
    req.headers = {{"Accept", "application/vnd.github+json"}, {"User-Agent", "zotero-for-kindle/" + appVersion().str()}};
    req.timeoutS = 30;
    req.maxBodyBytes = maxBytes;
    req.cancel = cancel;
    return req;
}

struct Asset {
    std::string url;
    int64_t size = 0;
};

// The named assets of a GitHub release JSON; nullopt if it isn't a release.
std::optional<std::pair<std::string, std::map<std::string, Asset>>> releaseAssets(const std::string& body) {
    const auto j = json::parse(body, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) return std::nullopt;
    std::map<std::string, Asset> assets;
    if (j.contains("assets") && j["assets"].is_array())
        for (const auto& a : j["assets"]) {
            if (!a.is_object() || !a.contains("name") || !a["name"].is_string() ||
                !a.contains("browser_download_url") || !a["browser_download_url"].is_string())
                continue;
            Asset x{a["browser_download_url"].get<std::string>()};
            if (a.contains("size") && a["size"].is_number_integer()) x.size = a["size"].get<int64_t>();
            assets[a["name"].get<std::string>()] = x;
        }
    return std::make_pair(j["tag_name"].get<std::string>(), std::move(assets));
}

bool cancelled(const std::shared_ptr<std::atomic<bool>>& c) { return c && *c; }

CheckResult failure(const HttpResponse& r) {
    if (r.curlCode != 0) return {CheckStatus::Offline, "Couldn't reach GitHub: " + r.curlError};
    if (r.status == 403 || r.status == 429) return {CheckStatus::Failed, "GitHub is busy. Try again in an hour."};
    return {CheckStatus::Failed, "GitHub answered HTTP " + std::to_string(r.status) + "."};
}

}  // namespace

std::string cacheDir(const std::string& pluginsDir) { return (fs::path(pluginsDir) / ".catalog").string(); }

Cache loadCache(const std::string& pluginsDir) {
    Cache c;
    const fs::path dir = cacheDir(pluginsDir);
    if (auto text = readFile(dir / "registry.json")) c.registry = parseRegistry(*text, nullptr);
    if (auto text = readFile(dir / "latest.json")) {
        const auto j = json::parse(*text, nullptr, false);
        try {
            if (j.is_object()) {
                c.registryMs = j.value("registryMs", int64_t{0});
                if (j.contains("plugins") && j["plugins"].is_object())
                    for (const auto& [id, p] : j["plugins"].items()) {
                        auto v = parseVersion(p.at("version").get<std::string>());
                        if (!v) continue;
                        c.latest[id] = {*v, p.at("zipUrl").get<std::string>(), p.at("sigUrl").get<std::string>(),
                                        p.at("zipSize").get<int64_t>(), p.at("checkedMs").get<int64_t>()};
                    }
            }
        } catch (const json::exception&) {
            c.registryMs = 0;
            c.latest.clear();
        }
    }
    if (auto text = readFile(dir / "installed.json")) {
        const auto j = json::parse(*text, nullptr, false);
        if (j.is_object())
            for (const auto& [id, v] : j.items())
                if (v.is_string() && v.get<std::string>() == "catalog") c.fromCatalog.insert(id);
    }
    return c;
}

bool saveCache(const std::string& pluginsDir, const Cache& c) {
    const fs::path dir = cacheDir(pluginsDir);
    json plugins = json::object();
    for (const auto& [id, l] : c.latest)
        plugins[id] = {{"version", l.version.str()}, {"zipUrl", l.zipUrl}, {"sigUrl", l.sigUrl},
                       {"zipSize", l.zipSize}, {"checkedMs", l.checkedMs}};
    json installed = json::object();
    for (const auto& id : c.fromCatalog) installed[id] = "catalog";
    const auto dump = [](const json& j) { return j.dump(1, ' ', false, json::error_handler_t::replace); };
    const bool a = writeAtomic(dir / "latest.json", dump({{"registryMs", c.registryMs}, {"plugins", plugins}}));
    const bool b = writeAtomic(dir / "installed.json", dump(installed));
    return a && b;
}

const Entry* find(const Cache& c, std::string_view id) {
    if (!c.registry) return nullptr;
    for (const auto& e : c.registry->plugins)
        if (e.id == id) return &e;
    return nullptr;
}

std::optional<Latest> updateFor(const Cache& c, std::string_view id, std::string_view installedVersion) {
    auto it = c.latest.find(std::string(id));
    if (it == c.latest.end()) return std::nullopt;
    const auto installed = parseVersion(installedVersion);
    if (!installed || it->second.version <= *installed) return std::nullopt;
    return it->second;
}

CheckResult runCheck(Http& http, const CheckOptions& opt, const std::string& pluginsDir,
                     const std::vector<std::string>& installedIds, bool force,
                     std::shared_ptr<std::atomic<bool>> cancel) {
    const CheckResult stopped{CheckStatus::Failed, "Cancelled."};
    if (cancelled(cancel)) return stopped;

    // The registry: its release, then registry.json and its signature.
    const HttpResponse rel = http.send(get(opt.feed, cancel, kMaxRegistryBytes));
    if (cancelled(cancel)) return stopped;
    if (rel.curlCode != 0 || rel.status != 200) return failure(rel);
    const auto release = releaseAssets(rel.body);
    if (!release || !release->second.count("registry.json") || !release->second.count("registry.json.sig"))
        return {CheckStatus::Failed, "The catalog's release has no registry.json."};
    const HttpResponse sig = http.send(get(release->second.at("registry.json.sig").url, cancel, 4096));
    if (cancelled(cancel)) return stopped;
    if (sig.curlCode != 0 || sig.status != 200) return failure(sig);
    const HttpResponse body = http.send(get(release->second.at("registry.json").url, cancel, kMaxRegistryBytes));
    if (cancelled(cancel)) return stopped;
    if (body.curlCode != 0 || body.status != 200) return failure(body);
    if (!verifySignature(body.body, sig.body, opt.registryKey))
        return {CheckStatus::Failed, "The catalog isn't signed with the app's key."};
    std::string why;
    auto registry = parseRegistry(body.body, &why);
    if (!registry) return {CheckStatus::Failed, "The catalog couldn't be read: " + why};

    const fs::path dir = cacheDir(pluginsDir);
    if (!writeAtomic(dir / "registry.json.sig", sig.body) || !writeAtomic(dir / "registry.json", body.body))
        return {CheckStatus::Failed, "Couldn't save the catalog."};
    Cache cache = loadCache(pluginsDir);
    cache.registry = std::move(registry);
    cache.registryMs = opt.wallMs();

    // Each installed plugin in the registry, at most every 6 h unless forced.
    for (const auto& id : installedIds) {
        const Entry* e = find(cache, id);
        if (!e) continue;
        auto old = cache.latest.find(id);
        const int64_t now = opt.wallMs();
        if (!force && old != cache.latest.end() && now - old->second.checkedMs < kPluginCheckEveryMs) continue;
        if (cancelled(cancel)) break;
        const HttpResponse r = http.send(get("https://api.github.com/repos/" + e->repo + "/releases/latest", cancel, kMaxRegistryBytes));
        if (r.curlCode != 0 || r.status != 200) continue;  // quietly: tried again next time
        const auto pr = releaseAssets(r.body);
        if (!pr) continue;
        const auto version = versionFromTag(pr->first);
        const auto zip = pr->second.find("plugin.zip"), zipSig = pr->second.find("plugin.zip.sig");
        if (!version || zip == pr->second.end() || zipSig == pr->second.end()) continue;
        cache.latest[id] = {*version, zip->second.url, zipSig->second.url, zip->second.size, now};
    }
    if (!saveCache(pluginsDir, cache)) return {CheckStatus::Failed, "Couldn't save the catalog."};
    if (cancelled(cancel)) return stopped;
    return {CheckStatus::Ok, ""};
}

}  // namespace zk::catalog
