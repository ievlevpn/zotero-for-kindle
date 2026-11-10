#pragma once
// Zotero Web API v3 for one personal library. Every call returns nullopt on failure and fills
// `err` (when given) with a plain-language UserError. Rate limiting (429/503 + Retry-After) is
// waited out up to three times; a Backoff header delays the next request.

#include "net/errors.h"
#include "net/http.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace zk {

struct KeyInfo {
    int64_t userId = 0;
    bool canRead = false, canWrite = false;
    bool canReadFiles = false;  // Zotero storage downloads
};

struct VersionMap {
    int64_t libraryVersion = 0;
    std::map<std::string, int64_t> versions;
};

struct Deleted {
    int64_t libraryVersion = 0;
    std::vector<std::string> items, collections, settings;
};

struct SettingsListing {
    int64_t libraryVersion = 0;
    std::map<std::string, std::pair<nlohmann::json, int64_t>> settings;  // key -> (value, version)
};

struct WriteResult {
    std::map<int, std::string> successKeys;  // batch index -> key
    std::map<int, nlohmann::json> successful;  // batch index -> the object as Zotero now has it (POST)
    std::map<int, std::string> failed;       // batch index -> message (PATCH, DELETE of one: index 0, a 400)
    std::map<int, int> failedCodes;          // batch index -> Zotero's code (e.g. 400, 412)
    bool preconditionFailed = false;         // 412: the library changed since `ifUnmodifiedSince`
    bool gone = false;                       // patchItem, deleteItem: 404, the item isn't in Zotero any more
    int64_t libraryVersion = 0;
};

class ZoteroApi {
public:
    ZoteroApi(Http&, int64_t userId, std::string apiKey);

    void setSleeper(std::function<void(int seconds)>);
    void setOnWait(std::function<void(const UserError&)>);  // "Zotero is busy — retrying at …"

    std::optional<KeyInfo> keyInfo(UserError*);
    // kind: "items" or "collections". Includes trashed items (their data has "deleted": 1).
    // itemType narrows an items listing, e.g. "annotation" or "-annotation".
    std::optional<VersionMap> versions(const std::string& kind, int64_t since, UserError*,
                                       const std::string& itemType = "");
    // Versions of one item's annotation children (e.g. an EPUB attachment's highlights).
    std::optional<VersionMap> annotationVersions(const std::string& parentKey, int64_t since, UserError*);
    std::optional<std::vector<nlohmann::json>> fetch(const std::string& kind, const std::vector<std::string>& keys /* ≤50 */,
                                                     UserError*);
    std::optional<Deleted> deleted(int64_t since, UserError*);
    std::optional<WriteResult> createItems(const nlohmann::json& array /* ≤50 */, int64_t ifUnmodifiedSince, UserError*);
    // DELETE /items?itemKey=… (≤50 keys). Success carries the new library version; 412 sets preconditionFailed.
    std::optional<WriteResult> deleteItems(const std::vector<std::string>& keys, int64_t ifUnmodifiedSince, UserError*);
    // DELETE /items/<key> against the object's own version: 204 carries the new library version;
    // 412 sets preconditionFailed, 404 gone, 400 failed[0] (Zotero's message).
    std::optional<WriteResult> deleteItem(const std::string& key, int64_t itemVersion, UserError*);
    // PATCH /items/<key> with only the fields in `data`, against the item's own version: 204
    // carries the new library version (the item's new version); 412 sets preconditionFailed, 404
    // gone, 400 or 413 (Zotero refused what was sent) failed[0] with Zotero's message.
    std::optional<WriteResult> patchItem(const std::string& key, const nlohmann::json& data, int64_t itemVersion,
                                         UserError*);
    // GET /settings?since=N: the user library's synced settings changed since N.
    std::optional<SettingsListing> settings(int64_t since, UserError*);
    // POST /settings {key: {value, version}}: 204 carries the new library version; a setting whose
    // version is stale (changed elsewhere) sets preconditionFailed.
    std::optional<WriteResult> writeSettings(const nlohmann::json& body, UserError*);

private:
    HttpResponse send(HttpRequest req, UserError* err);
    std::string userUrl(const std::string& path) const;

    Http& http_;
    int64_t userId_;
    std::string apiKey_;
    std::function<void(int)> sleeper_;
    std::function<void(const UserError&)> onWait_;
    int backoffS_ = 0;
};

}  // namespace zk
