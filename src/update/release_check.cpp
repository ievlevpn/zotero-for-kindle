#include "update/release_check.h"

#include <nlohmann/json.hpp>

#include <regex>

namespace zk {

const char* const kDefaultFeed = "https://api.github.com/repos/ievlevpn/zotero-for-kindle/releases/latest";

namespace {

UserError unreadable(const std::string& details) {
    return {ErrorKind::ServerError, "Couldn't read the update information.", details};
}

}  // namespace

CheckResult checkLatest(Http& http, const std::string& feedUrl, Version current, std::function<NetState()> probe) {
    CheckResult result;
    HttpRequest req;
    req.url = feedUrl;
    req.headers = {{"Accept", "application/vnd.github+json"}, {"User-Agent", "zotero-for-kindle/" + current.str()}};
    req.timeoutS = 30;
    const HttpResponse r = http.send(req);
    if (r.curlCode == 0 && (r.status == 403 || r.status == 429)) {
        auto left = r.headers.find("x-ratelimit-remaining");
        if (r.status == 429 || (left != r.headers.end() && left->second == "0")) {
            result.error = {ErrorKind::RateLimited, "GitHub is busy. Try again in an hour.",
                            "HTTP " + std::to_string(r.status)};
            return result;
        }
    }
    if (r.curlCode != 0 || r.status != 200) {
        result.error = classify(r, r.curlCode ? probe() : NetState{}, hostOf(feedUrl), false);
        return result;
    }
    const auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name") || !j["tag_name"].is_string()) {
        result.error = unreadable("not a release");
        return result;
    }
    auto version = parseVersion(j["tag_name"].get<std::string>());
    if (j.contains("body") && j["body"].is_string()) result.release.notes = plainNotes(j["body"].get<std::string>());
    if (j.contains("assets") && j["assets"].is_array())
        for (const auto& a : j["assets"]) {
            if (!a.is_object() || !a.contains("name") || !a["name"].is_string() ||
                !a.contains("browser_download_url") || !a["browser_download_url"].is_string())
                continue;
            const std::string name = a["name"], url = a["browser_download_url"];
            if (name == "zotero-for-kindle.zip") {
                result.release.zipUrl = url;
                if (a.contains("size") && a["size"].is_number_integer()) result.release.zipSize = a["size"].get<int64_t>();
            } else if (name == "update.json") {
                result.release.manifestUrl = url;
            } else if (name == "update.json.sig") {
                result.release.sigUrl = url;
            }
        }
    if (!version) {
        result.kind = CheckResult::Kind::NotInstallable;
        return result;
    }
    result.release.version = *version;
    if (*version <= current) {
        result.kind = CheckResult::Kind::UpToDate;
        return result;
    }
    const bool complete = !result.release.zipUrl.empty() && !result.release.manifestUrl.empty() &&
                          !result.release.sigUrl.empty() && result.release.zipSize > 0;
    result.kind = complete ? CheckResult::Kind::Available : CheckResult::Kind::NotInstallable;
    return result;
}

std::string plainNotes(const std::string& markdown) {
    std::string s = std::regex_replace(markdown, std::regex("\r"), "");
    s = std::regex_replace(s, std::regex("<!--[\\s\\S]*?-->"), "");
    s = std::regex_replace(s, std::regex("\\[([^\\]]*)\\]\\([^)]*\\)"), "$1");
    s = std::regex_replace(s, std::regex("(^|\n)#+ *"), "$1");
    s = std::regex_replace(s, std::regex("(^|\n)[-*] +"), "$1• ");
    s = std::regex_replace(s, std::regex("\\*\\*|\\*|`|__"), "");
    s = std::regex_replace(s, std::regex("[ \t]+\n"), "\n");
    s = std::regex_replace(s, std::regex("\n{3,}"), "\n\n");
    const size_t b = s.find_first_not_of("\n "), e = s.find_last_not_of("\n ");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

}  // namespace zk
