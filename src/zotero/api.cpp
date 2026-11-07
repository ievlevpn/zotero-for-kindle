#include "zotero/api.h"

#include "core/log.h"

#include <chrono>
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
        if (r.curlCode != 0 || r.status < 200 || r.status >= 300) {
            if (!(r.status == 412 && req.method == "POST")) {
                UserError e = classify(r, r.curlCode ? probeNetState() : NetState{}, kHost, false, req.method);
                log(LogLevel::Warn, "zotero: %s %s -> %s (%s)", req.method.c_str(), req.url.c_str(), e.message.c_str(),
                    e.details.c_str());
                setError(err, e);
            }
        }
        return r;
    }
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
    return info;
}

std::optional<VersionMap> ZoteroApi::versions(const std::string& kind, int64_t since, UserError* err,
                                              const std::string& itemType) {
    HttpRequest req;
    req.url = userUrl("/" + kind + "?since=" + std::to_string(since) + "&format=versions" +
                      (kind == "items" ? "&includeTrashed=1" : "") + (itemType.empty() ? "" : "&itemType=" + itemType));
    HttpResponse r = send(req, err);
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
    req.url = userUrl("/items/" + parentKey + "/children?format=versions&itemType=annotation&includeTrashed=1" +
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
    req.url = userUrl("/" + kind + "?" + param + "=" + joinKeys(keys) + "&format=json" +
                      (kind == "items" ? "&includeTrashed=1" : ""));
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_array()) return setError(err, badBody(kind + ": not an array")), std::nullopt;
    return std::vector<json>(j.begin(), j.end());
}

std::optional<Deleted> ZoteroApi::deleted(int64_t since, UserError* err) {
    HttpRequest req;
    req.url = userUrl("/deleted?since=" + std::to_string(since));
    HttpResponse r = send(req, err);
    if (r.curlCode || r.status != 200) return std::nullopt;
    auto j = json::parse(r.body, nullptr, false);
    if (!j.is_object()) return setError(err, badBody("deleted: not an object")), std::nullopt;
    Deleted out;
    out.libraryVersion = libraryVersionOf(r);
    for (const char* kind : {"items", "collections"}) {
        auto& dst = std::string(kind) == "items" ? out.items : out.collections;
        if (j.contains(kind) && j[kind].is_array())
            for (const auto& k : j[kind])
                if (k.is_string()) dst.push_back(k.get<std::string>());
    }
    return out;
}

std::optional<WriteResult> ZoteroApi::createItems(const json& array, int64_t ifUnmodifiedSince, UserError* err) {
    HttpRequest req;
    req.method = "POST";
    req.url = userUrl("/items");
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
    if (j.contains("failed") && j["failed"].is_object())
        for (auto it = j["failed"].begin(); it != j["failed"].end(); ++it)
            out.failed[index(it.key())] = it.value().is_object() ? it.value().value("message", std::string("failed")) : "failed";
    return out;
}

}  // namespace zk
