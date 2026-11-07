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

// Why the request must not go out, or empty if it may.
std::string refusal(const HttpRequest& req, const std::function<bool(const std::string&)>& ours) {
    const std::string& m = req.method;
    if (m == "GET" || m == "HEAD" || m == "PROPFIND")
        return req.body.empty() ? std::string{} : m + " with a body";  // curl would send it as a POST
    if (m == "DELETE") return "DELETE is never sent";
    if (!isZoteroApi(req.url)) return m + " outside the Zotero API";

    static const std::regex itemPath(R"(^/users/\d+/items/([A-Z0-9]{8})/?$)");
    static const std::regex itemsPath(R"(^/users/\d+/items/?$)");
    const std::string path = pathOf(req.url);
    std::smatch match;
    if ((m == "PUT" || m == "PATCH") && std::regex_match(path, match, itemPath)) {
        return ours(match[1].str()) ? std::string{} : "item " + match[1].str() + " was not created by this app";
    }
    if (m == "POST" && std::regex_match(path, itemsPath)) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_array()) return "POST body is not a JSON array";
        for (const auto& obj : body) {
            if (!obj.is_object()) return "POST body holds a non-object";
            if (!obj.contains("key")) continue;
            if (!obj["key"].is_string()) return "POST object has a non-string key";
            const std::string key = obj["key"].get<std::string>();
            if (!ours(key)) return "item " + key + " was not created by this app";
        }
        return {};
    }
    return m + " " + path + " is not an allowed write";
}

}  // namespace

WriteGuard::WriteGuard(Http& inner, std::function<bool(const std::string& key)> createdByUs)
    : inner_(inner), createdByUs_(std::move(createdByUs)) {}

HttpResponse WriteGuard::send(const HttpRequest& req) {
    const std::string why = refusal(req, createdByUs_);
    if (why.empty()) return inner_.send(req);
    log(LogLevel::Warn, "write guard refused %s %s: %s", req.method.c_str(), pathOf(req.url).c_str(), why.c_str());
    HttpResponse r;
    r.curlCode = -1;
    r.curlError = "blocked by write guard: " + why;
    return r;
}

}  // namespace zk
