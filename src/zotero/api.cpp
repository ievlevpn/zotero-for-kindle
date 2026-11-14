#include "zotero/api.h"

#include "core/log.h"

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

namespace zk {

using nlohmann::json;

namespace {

constexpr const char* kBase = "https://api.zotero.org";
constexpr const char* kHost = "api.zotero.org";
constexpr int kMaxRateLimitRetries = 3;

int headerInt(const HttpResponse& r, const char* name, int fallback = 0) {
    auto it = r.headers.find(name);
    if (it == r.headers.end()) return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

int64_t libraryVersionOf(const HttpResponse& r) {
    auto it = r.headers.find("last-modified-version");
    if (it == r.headers.end()) return 0;
    try {
        return std::stoll(it->second);
    } catch (...) {
        return 0;
    }
}

std::string writeToken() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s(32, '0');
    for (auto& c : s) c = hex[rng() & 15];
    return s;
}

std::string joinKeys(const std::vector<std::string>& keys) {
    std::string s;
    for (const auto& k : keys) s += (s.empty() ? "" : ",") + k;
    return s;
}

void setError(UserError* err, UserError e) {
    if (err) *err = std::move(e);
}

UserError badBody(const std::string& what) {
    return {ErrorKind::ServerError, "Zotero sent an answer the app doesn't understand.", what};
}

}  // namespace

ZoteroApi::ZoteroApi(Http& http, int64_t userId, std::string apiKey)
    : http_(http), userId_(userId), apiKey_(std::move(apiKey)),
      sleeper_([](int s) { std::this_thread::sleep_for(std::chrono::seconds(s)); }) {}

void ZoteroApi::setSleeper(std::function<void(int)> f) { sleeper_ = std::move(f); }
void ZoteroApi::setOnWait(std::function<void(const UserError&)> f) { onWait_ = std::move(f); }

std::string ZoteroApi::userUrl(const std::string& path) const {
    return std::string(kBase) + "/users/" + std::to_string(userId_) + path;
}

std::string ZoteroApi::libraryUrl(const std::string& path) const {
    if (library_ == 0) return userUrl(path);
    return std::string(kBase) + "/groups/" + std::to_string(library_) + path;
}

KeyInfo::Access KeyInfo::groupAccess(int64_t groupId) const {
    if (auto it = groups.find(groupId); it != groups.end()) return it->second;
    return allGroups.value_or(Access{});
}

bool KeyInfo::anyGroupAccess() const {
    if (allGroups && allGroups->read) return true;
    for (const auto& [id, a] : groups)
        if (a.read) return true;
    return false;
}

HttpResponse ZoteroApi::send(HttpRequest req, UserError* err) {
    req.headers.emplace_back("Zotero-API-Version", "3");
    req.headers.emplace_back("Zotero-API-Key", apiKey_);
    for (int attempt = 0;; ++attempt) {
        if (backoffS_ > 0) {
            sleeper_(backoffS_);
            backoffS_ = 0;
        }
        HttpResponse r = http_.send(req);
        backoffS_ = headerInt(r, "backoff");
        const bool busy = r.curlCode == 0 && (r.status == 429 || (r.status == 503 && r.headers.count("retry-after")));
        if (busy && attempt < kMaxRateLimitRetries) {
            UserError wait = classify(r, NetState{}, kHost, false, req.method);
            log(LogLevel::Info, "zotero: %s", wait.message.c_str());
            if (onWait_) onWait_(wait);
            sleeper_(headerInt(r, "retry-after", 60));
            continue;
        }
        if (r.curlCode != 0 || ((r.status < 200 || r.status >= 300) && r.status != 304)) {
            if (!(r.status == 412 && (req.method == "POST" || req.method == "DELETE" || req.method == "PATCH"))) {
                UserError e = classify(r, r.curlCode ? probeNetState() : NetState{}, kHost, false, req.method);
                log(LogLevel::Warn, "zotero: %s %s -> %s (%s)", req.method.c_str(), req.url.c_str(), e.message.c_str(),
                    e.details.c_str());
                setError(err, e);
            }
        }
        return r;
    }
}

std::string ZoteroApi::keyFingerprint() const {
    uint64_t h = 1469598103934665603ull;  // FNV-1a 64
    for (unsigned char c : std::to_string(userId_) + ":" + apiKey_) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

std::optional<KeyInfo> ZoteroApi::keyInfo(UserError* err) {
    HttpRequest req;
    req.url = std::string(kBase) + "/keys/current";
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("keys/current: not JSON")), std::nullopt;
    KeyInfo info;
    info.userId = j.value("userID", int64_t{0});
    const json user = j.contains("access") && j["access"].is_object() ? j["access"].value("user", json::object()) : json::object();
    info.canRead = user.is_object() && user.value("library", false);
    info.canWrite = user.is_object() && user.value("write", false);
    info.canReadFiles = user.is_object() && user.value("files", false);
    if (j.contains("username") && j["username"].is_string()) info.username = j["username"].get<std::string>();
    if (j.contains("displayName") && j["displayName"].is_string()) info.displayName = j["displayName"].get<std::string>();
    // {"access": {"groups": {"all": {"library": true, "write": false}, "12345": {...}}}}
    const json groups = j.contains("access") && j["access"].is_object() ? j["access"].value("groups", json::object())
                                                                         : json::object();
    if (groups.is_object())
        for (const auto& [id, a] : groups.items()) {
            if (!a.is_object()) continue;
            KeyInfo::Access access{a.value("library", false), a.value("write", false)};
            if (id == "all") {
                info.allGroups = access;
                continue;
            }
            try {
                info.groups[std::stoll(id)] = access;
            } catch (...) {
            }
        }
    return info;
}

std::optional<std::map<int64_t, int64_t>> ZoteroApi::groupVersions(UserError* err) {
    HttpRequest req;
    req.url = userUrl("/groups?format=versions");
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("groups versions: not an object")), std::nullopt;
    std::map<int64_t, int64_t> out;
    for (const auto& [id, v] : j.items()) {
        try {
            const int64_t group = std::stoll(id);
            if (v.is_number_integer()) out[group] = v.get<int64_t>();
            else if (v.is_string()) out[group] = std::stoll(v.get<std::string>());
        } catch (...) {
        }
    }
    return out;
}

std::optional<GroupInfo> ZoteroApi::group(int64_t groupId, UserError* err) {
    HttpRequest req;
    req.url = std::string(kBase) + "/groups/" + std::to_string(groupId);
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("group: not an object")), std::nullopt;
    const json d = j.contains("data") && j["data"].is_object() ? j["data"] : json::object();
    auto num = [](const json& o, const char* k) -> int64_t {
        auto it = o.find(k);
        return it != o.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
    };
    GroupInfo g;
    g.id = num(d, "id") ? num(d, "id") : groupId;
    g.version = num(j, "version") ? num(j, "version") : num(d, "version");
    g.owner = num(d, "owner");
    g.name = d.value("name", "");
    g.libraryEditing = d.value("libraryEditing", "admins");
    g.fileEditing = d.value("fileEditing", "admins");
    if (auto it = d.find("admins"); it != d.end() && it->is_array())
        for (const auto& a : *it)
            if (a.is_number_integer()) g.admins.push_back(a.get<int64_t>());
    return g;
}

std::optional<VersionMap> ZoteroApi::versions(const std::string& kind, int64_t since, UserError* err,
                                              const std::string& itemType, int64_t ifModifiedSince) {
    HttpRequest req;
    req.url = libraryUrl("/" + kind + "?since=" + std::to_string(since) + "&format=versions" +
                      (kind == "items" ? "&includeTrashed=1" : "") + (itemType.empty() ? "" : "&itemType=" + itemType));
    if (ifModifiedSince > 0) req.headers.emplace_back("If-Modified-Since-Version", std::to_string(ifModifiedSince));
    HttpResponse r = send(req, err);
    if (!r.curlCode && r.status == 304 && ifModifiedSince > 0) {
        VersionMap unchanged;
        unchanged.notModified = true;
        unchanged.libraryVersion = ifModifiedSince;
        return unchanged;
    }
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody(kind + " versions: not an object")), std::nullopt;
    VersionMap out;
    out.libraryVersion = libraryVersionOf(r);
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_number_integer()) out.versions[it.key()] = it.value().get<int64_t>();
    return out;
}

std::optional<VersionMap> ZoteroApi::annotationVersions(const std::string& parentKey, int64_t since, UserError* err) {
    HttpRequest req;
    req.url = libraryUrl("/items/" + parentKey + "/children?format=versions&itemType=annotation&includeTrashed=1" +
                      (since > 0 ? "&since=" + std::to_string(since) : ""));
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("children versions: not an object")), std::nullopt;
    VersionMap out;
    out.libraryVersion = libraryVersionOf(r);
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_number_integer()) out.versions[it.key()] = it.value().get<int64_t>();
    return out;
}

std::optional<std::vector<json>> ZoteroApi::fetch(const std::string& kind, const std::vector<std::string>& keys,
                                                  UserError* err) {
    if (keys.empty()) return std::vector<json>{};
    HttpRequest req;
    const std::string param = kind == "collections" ? "collectionKey" : "itemKey";
    req.url = libraryUrl("/" + kind + "?" + param + "=" + joinKeys(keys) + "&format=json" +
                      (kind == "items" ? "&includeTrashed=1" : ""));
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_array()) return setError(err, badBody(kind + ": not an array")), std::nullopt;
    return std::vector<json>(j.begin(), j.end());
}

std::optional<Deleted> ZoteroApi::deleted(int64_t since, UserError* err) {
    HttpRequest req;
    req.url = libraryUrl("/deleted?since=" + std::to_string(since));
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("deleted: not an object")), std::nullopt;
    Deleted out;
    out.libraryVersion = libraryVersionOf(r);
    for (const char* kind : {"items", "collections", "settings"}) {
        const std::string k = kind;
        auto& dst = k == "items" ? out.items : k == "collections" ? out.collections : out.settings;
        if (j.contains(kind) && j[kind].is_array())
            for (const auto& k : j[kind])
                if (k.is_string()) dst.push_back(k.get<std::string>());
    }
    return out;
}

std::optional<WriteResult> ZoteroApi::createItems(const json& array, int64_t ifUnmodifiedSince, UserError* err) {
    HttpRequest req;
    req.method = "POST";
    req.url = libraryUrl("/items");
    req.body = array.dump();
    req.headers = {{"Content-Type", "application/json"},
                   {"Zotero-Write-Token", writeToken()},
                   {"If-Unmodified-Since-Version", std::to_string(ifUnmodifiedSince)}};
    HttpResponse r = send(req, err);
    WriteResult out;
    out.libraryVersion = libraryVersionOf(r);
    if (r.curlCode == 0 && r.status == 412) {
        out.preconditionFailed = true;
        return out;
    }
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("write: not an object")), std::nullopt;
    auto index = [](const std::string& s) {
        try {
            return std::stoi(s);
        } catch (...) {
            return -1;
        }
    };
    if (j.contains("success") && j["success"].is_object())
        for (auto it = j["success"].begin(); it != j["success"].end(); ++it)
            if (it.value().is_string()) out.successKeys[index(it.key())] = it.value().get<std::string>();
    if (j.contains("successful") && j["successful"].is_object())
        for (auto it = j["successful"].begin(); it != j["successful"].end(); ++it)
            if (it.value().is_object()) out.successful[index(it.key())] = it.value();
    if (j.contains("failed") && j["failed"].is_object())
        for (auto it = j["failed"].begin(); it != j["failed"].end(); ++it)
        {
            out.failed[index(it.key())] = it.value().is_object() ? it.value().value("message", std::string("failed")) : "failed";
            if (it.value().is_object() && it.value().contains("code") && it.value()["code"].is_number_integer())
                out.failedCodes[index(it.key())] = it.value()["code"].get<int>();
        }
    return out;
}

std::optional<SettingsListing> ZoteroApi::settings(int64_t since, UserError* err) {
    HttpRequest req;
    req.url = userUrl("/settings?since=" + std::to_string(since));
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("settings: not JSON")), std::nullopt;
    SettingsListing out;
    out.libraryVersion = libraryVersionOf(r);
    for (const auto& [key, s] : j.items())
        if (s.is_object() && s.contains("value") && s.contains("version") && s["version"].is_number_integer())
            out.settings[key] = {s["value"], s["version"].get<int64_t>()};
    return out;
}

std::optional<WriteResult> ZoteroApi::writeSettings(const json& body, UserError* err) {
    HttpRequest req;
    req.method = "POST";
    req.url = userUrl("/settings");
    req.headers = {{"Content-Type", "application/json"}};
    req.body = body.dump();
    HttpResponse r = send(req, err);
    WriteResult out;
    out.libraryVersion = libraryVersionOf(r);
    if (r.curlCode == 0 && r.status == 412) {
        out.preconditionFailed = true;
        return out;
    }
    if (r.curlCode || r.status != 204) return std::nullopt;
    return out;
}

std::optional<WriteResult> ZoteroApi::patchItem(const std::string& key, const json& data, int64_t itemVersion,
                                                UserError* err) {
    HttpRequest req;
    req.method = "PATCH";
    req.url = libraryUrl("/items/" + key);
    req.body = data.dump();
    req.headers = {{"Content-Type", "application/json"},
                   {"If-Unmodified-Since-Version", std::to_string(itemVersion)}};
    HttpResponse r = send(req, err);
    WriteResult out;
    out.libraryVersion = libraryVersionOf(r);
    if (r.curlCode == 0 && r.status == 412) {
        out.preconditionFailed = true;
        return out;
    }
    if (r.curlCode == 0 && r.status == 404) {
        out.gone = true;
        return out;
    }
    if (r.curlCode == 0 && (r.status == 400 || r.status == 413)) {  // what was sent is refused, not the connection
        out.failed[0] = r.body.empty() ? "HTTP " + std::to_string(r.status) : r.body;
        out.failedCodes[0] = static_cast<int>(r.status);
        return out;
    }
    if (r.curlCode || r.status != 204) return std::nullopt;
    return out;
}

std::optional<WriteResult> ZoteroApi::deleteItem(const std::string& key, int64_t itemVersion, UserError* err) {
    HttpRequest req;
    req.method = "DELETE";
    req.url = libraryUrl("/items/" + key);
    req.headers = {{"If-Unmodified-Since-Version", std::to_string(itemVersion)}};
    HttpResponse r = send(req, err);
    WriteResult out;
    out.libraryVersion = libraryVersionOf(r);
    if (r.curlCode == 0 && r.status == 412) {
        out.preconditionFailed = true;
        return out;
    }
    if (r.curlCode == 0 && r.status == 404) {
        out.gone = true;
        return out;
    }
    if (r.curlCode == 0 && r.status == 400) {
        out.failed[0] = r.body.empty() ? "HTTP 400" : r.body;
        out.failedCodes[0] = 400;
        return out;
    }
    if (r.curlCode || r.status != 204) return std::nullopt;
    return out;
}

std::optional<WriteResult> ZoteroApi::deleteItems(const std::vector<std::string>& keys, int64_t ifUnmodifiedSince,
                                                  UserError* err) {
    HttpRequest req;
    req.method = "DELETE";
    req.url = libraryUrl("/items?itemKey=" + joinKeys(keys));
    req.headers = {{"If-Unmodified-Since-Version", std::to_string(ifUnmodifiedSince)}};
    HttpResponse r = send(req, err);
    WriteResult out;
    out.libraryVersion = libraryVersionOf(r);
    if (r.curlCode == 0 && r.status == 412) {
        out.preconditionFailed = true;
        return out;
    }
    if (r.curlCode || r.status != 204) return std::nullopt;
    return out;
}

}  // namespace zk
