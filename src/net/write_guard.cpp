#include "net/write_guard.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <regex>

namespace zk {

namespace {

// The URL's path without scheme, host and query: "https://h:1/a/b?c" -> "/a/b".
std::string pathOf(const std::string& url) {
    size_t start = 0;
    if (size_t scheme = url.find("://"); scheme != std::string::npos) {
        start = url.find('/', scheme + 3);
        if (start == std::string::npos) return "/";
    }
    size_t end = url.find_first_of("?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool isZoteroApi(const std::string& url) { return url.rfind("https://api.zotero.org/", 0) == 0; }

// The URL's query: "https://h/a?b=c#d" -> "b=c".
std::string queryOf(const std::string& url) {
    size_t q = url.find('?');
    if (q == std::string::npos) return {};
    size_t end = url.find('#', q);
    return url.substr(q + 1, end == std::string::npos ? std::string::npos : end - q - 1);
}

using IsAnnotation = std::function<bool(const std::string&)>;

// Why `DELETE /users/<id>/items?itemKey=K1,K2` must not go out, or empty if every key is an annotation.
std::string bulkDeleteRefusal(const std::string& query, const IsAnnotation& isAnnotation) {
    static const std::regex keys(R"(^itemKey=([A-Z0-9]{8}(,[A-Z0-9]{8})*)$)");
    std::smatch match;
    if (!std::regex_match(query, match, keys)) return "DELETE needs exactly one itemKey= list";
    const std::string list = match[1].str();
    for (size_t i = 0; i < list.size(); i += 9) {
        const std::string key = list.substr(i, 8);
        if (!isAnnotation(key)) return "item " + key + " is not an annotation";
    }
    return {};
}

// POST /users/<id>/settings: only {"lastPageIndex_u_<file key>": {"value": <page or CFI>, "version": n}}.
std::string settingsRefusal(const HttpRequest& req, const IsAnnotation& isAttachment) {
    if (!isAttachment) return "settings are not writable here";
    if (!queryOf(req.url).empty()) return "POST settings with a query";
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (!body.is_object() || body.empty()) return "POST settings body is not a non-empty object";
    static const std::regex positionKey(R"(^lastPageIndex_u_([A-Z0-9]{8})$)");
    for (const auto& [key, setting] : body.items()) {
        std::smatch match;
        if (!std::regex_match(key, match, positionKey)) return "setting " + key + " is not a reading position";
        if (!isAttachment(match[1].str())) return "setting " + key + " is not for a file of this library";
        if (!setting.is_object()) return "setting " + key + " is not an object";
        for (const auto& [field, v] : setting.items())
            if (field != "value" && field != "version") return "setting " + key + " has field " + field;
        // Always against the version it is based on (never a blind overwrite).
        if (!setting.contains("version") || !setting["version"].is_number_unsigned())
            return "setting " + key + " has no version";
        // A page index, or a CFI.
        const auto& value = setting.contains("value") ? setting["value"] : nlohmann::json();
        const bool page = value.is_number_unsigned();
        const bool cfi = value.is_string() && value.get<std::string>().rfind("/", 0) == 0 &&
                         value.get<std::string>().size() <= 2048;
        if (!page && !cfi) return "setting " + key + " has no position value";
    }
    return {};
}

// Why the request must not go out, or empty if it may.
std::string refusal(const HttpRequest& req, const IsAnnotation& isAnnotation, const IsAnnotation& isAttachment) {
    const std::string& m = req.method;
    if (m == "GET" || m == "HEAD" || m == "PROPFIND")
        return req.body.empty() ? std::string{} : m + " with a body";  // curl would send it as a POST
    if (!isZoteroApi(req.url)) return m + " outside the Zotero API";

    static const std::regex itemPath(R"(^/users/\d+/items/([A-Z0-9]{8})/?$)");
    static const std::regex itemsPath(R"(^/users/\d+/items/?$)");
    const std::string path = pathOf(req.url);
    const std::string query = queryOf(req.url);
    std::smatch match;
    if (m == "DELETE") {
        if (!req.body.empty()) return "DELETE with a body";
        if (std::regex_match(path, match, itemPath) && query.empty())
            return isAnnotation(match[1].str()) ? std::string{} : "item " + match[1].str() + " is not an annotation";
        if (std::regex_match(path, itemsPath)) return bulkDeleteRefusal(query, isAnnotation);
        return "DELETE " + path + " is not an allowed write";
    }
    if ((m == "PUT" || m == "PATCH") && std::regex_match(path, match, itemPath)) {
        return isAnnotation(match[1].str()) ? std::string{} : "item " + match[1].str() + " is not an annotation";
    }
    static const std::regex settingsPath(R"(^/users/\d+/settings/?$)");
    if (m == "POST" && std::regex_match(path, settingsPath)) return settingsRefusal(req, isAttachment);
    if (m == "POST" && std::regex_match(path, itemsPath)) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_array()) return "POST body is not a JSON array";
        for (const auto& obj : body) {
            if (!obj.is_object()) return "POST body holds a non-object";
            auto type = obj.find("itemType");
            if (type == obj.end() || *type != "annotation") return "POST object is not an annotation";
            if (!obj.contains("key")) continue;
            if (!obj["key"].is_string()) return "POST object has a non-string key";
            const std::string key = obj["key"].get<std::string>();
            if (!isAnnotation(key)) return "item " + key + " is not an annotation";
        }
        return {};
    }
    return m + " " + path + " is not an allowed write";
}

}  // namespace

WriteGuard::WriteGuard(Http& inner, std::function<bool(const std::string& key)> isAnnotation,
                       std::function<bool(const std::string& key)> isAttachment)
    : inner_(inner), isAnnotation_(std::move(isAnnotation)), isAttachment_(std::move(isAttachment)) {}

HttpResponse WriteGuard::send(const HttpRequest& req) {
    const std::string why = refusal(req, isAnnotation_, isAttachment_);
    if (why.empty()) return inner_.send(req);
    log(LogLevel::Warn, "write guard refused %s %s: %s", req.method.c_str(), pathOf(req.url).c_str(), why.c_str());
    HttpResponse r;
    r.curlCode = -1;
    r.curlError = "blocked by write guard: " + why;
    return r;
}

}  // namespace zk
