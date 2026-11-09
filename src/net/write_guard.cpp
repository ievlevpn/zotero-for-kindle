#include "net/write_guard.h"

#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <regex>
#include <set>

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

// Sent against a version: If-Unmodified-Since-Version: <number>.
bool versioned(const HttpRequest& req) {
    static const std::regex version(R"(^\d+$)");
    for (const auto& [name, value] : req.headers)
        if (name == "If-Unmodified-Since-Version" && std::regex_match(value, version)) return true;
    return false;
}

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

// PATCH /users/<id>/items/<key> of a regular item: only {"tags": [{"tag": …, "type": n}, …]}, against
// the item's version, adding or removing one tag name and keeping the others.
std::string tagsRefusal(const HttpRequest& req, const std::string& key, const WriteGuard::ItemTags& itemTags) {
    if (!itemTags) return "item " + key + " is not an annotation";
    const auto have = itemTags(key);
    if (!have) return "item " + key + " is not an item of this library";
    if (req.method != "PATCH") return "item " + key + ": only PATCH of its tags";
    if (!queryOf(req.url).empty()) return "PATCH item with a query";
    if (!versioned(req)) return "PATCH item " + key + " without If-Unmodified-Since-Version";
    auto body = nlohmann::json::parse(req.body, nullptr, false);
    if (!body.is_object() || body.size() != 1 || !body.contains("tags") || !body["tags"].is_array())
        return "PATCH item " + key + " changes more than its tags";
    std::set<std::string> after;
    for (const auto& t : body["tags"]) {
        if (!t.is_object() || !t.contains("tag") || !t["tag"].is_string() || t["tag"].get<std::string>().empty())
            return "PATCH item " + key + " has a malformed tag";
        for (const auto& [field, v] : t.items())
            if (field != "tag" && !(field == "type" && v.is_number_unsigned()))
                return "PATCH item " + key + " has tag field " + field;
        after.insert(t["tag"].get<std::string>());
    }
    const std::set<std::string> before(have->begin(), have->end());
    std::vector<std::string> changed;
    std::set_symmetric_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(changed));
    if (changed.size() != 1) return "PATCH item " + key + " changes " + std::to_string(changed.size()) + " tags, not one";
    return {};
}

bool isKey(const nlohmann::json& v) {
    static const std::regex key(R"(^[A-Z0-9]{8}$)");
    return v.is_string() && std::regex_match(v.get<std::string>(), key);
}

// The pending change of that kind on the key that `match` accepts, or null.
const PluginChange* pendingOf(const PluginChangeShape& shape, PluginChange::Kind kind,
                              const std::function<bool(const PluginChange&)>& match = nullptr) {
    for (const auto& c : shape.pending)
        if (c.kind == kind && c.state == PluginChange::State::Pending && (!match || match(c))) return &c;
    return nullptr;
}

// The fields a plugin's new item may carry: what Library makes of zk.zotero.createItem's fields.
bool createField(const std::string& f) {
    static const std::set<std::string> fields = {
        "key", "version", "itemType", "title", "creators", "date", "url", "abstractNote", "publicationTitle",
        "bookTitle", "proceedingsTitle", "websiteTitle", "blogTitle", "repository", "encyclopediaTitle",
        "dictionaryTitle", "DOI", "ISBN", "archiveID", "extra", "collections", "tags"};
    return fields.count(f) > 0;
}

// POST /items, one object that isn't an annotation: a plugin's pending CreateItem or AddNote, exactly.
std::string pluginPostRefusal(const nlohmann::json& obj, const WriteGuard::PluginChanges& pluginChanges) {
    using K = PluginChange::Kind;
    if (!pluginChanges) return "POST object is not an annotation";
    if (!obj.contains("key") || !isKey(obj["key"])) return "POST of a new item without its key";
    const std::string key = obj["key"].get<std::string>();
    const auto shape = pluginChanges(key);
    if (!shape) return "item " + key + ": no plugin change pending";
    if (obj.contains("version") && !(obj["version"].is_number_integer() && obj["version"].get<int64_t>() == 0))
        return "item " + key + ": a plugin's POST is of a new object (version 0)";
    const std::string type = obj.contains("itemType") && obj["itemType"].is_string() ? obj["itemType"].get<std::string>() : "";
    if (type == "note") {
        for (const auto& [field, v] : obj.items())
            if (field != "key" && field != "version" && field != "itemType" && field != "parentItem" && field != "note")
                return "note " + key + " has field " + field;
        if (!obj.contains("parentItem") || !isKey(obj["parentItem"]) || !obj.contains("note") || !obj["note"].is_string())
            return "note " + key + " is not a child note";
        const std::string parent = obj["parentItem"].get<std::string>(), html = obj["note"].get<std::string>();
        if (!pendingOf(*shape, K::AddNote, [&](const PluginChange& c) { return c.parentKey == parent && c.value == html; }))
            return "note " + key + " is not the note a plugin is adding";
        return {};
    }
    if (type.empty() || type == "attachment" || type == "annotation") return "item " + key + " has item type '" + type + "'";
    if (!pendingOf(*shape, K::CreateItem)) return "item " + key + " is not an item a plugin is creating";
    for (const auto& [field, v] : obj.items())
        if (!createField(field)) return "item " + key + " has field " + field;
    if (!obj.contains("title") || !obj["title"].is_string()) return "item " + key + " has no title";
    if (obj.contains("collections")) {
        if (!obj["collections"].is_array()) return "item " + key + ": collections is not a list";
        for (const auto& c : obj["collections"])
            if (!isKey(c)) return "item " + key + " has a malformed collection";
    }
    if (obj.contains("tags")) {
        if (!obj["tags"].is_array()) return "item " + key + ": tags is not a list";
        for (const auto& t : obj["tags"]) {
            if (!t.is_object() || t.size() != 1 || !t.contains("tag") || !t["tag"].is_string() ||
                t["tag"].get<std::string>().empty())
                return "item " + key + " has a malformed tag";
        }
    }
    if (obj.contains("creators")) {
        if (!obj["creators"].is_array()) return "item " + key + ": creators is not a list";
        for (const auto& c : obj["creators"]) {
            if (!c.is_object()) return "item " + key + " has a malformed creator";
            for (const auto& [field, v] : c.items())
                if ((field != "creatorType" && field != "firstName" && field != "lastName" && field != "name") || !v.is_string())
                    return "item " + key + " has creator field " + field;
        }
    }
    // Exactly what the pending change holds.
    nlohmann::json sent = obj;
    sent.erase("key");
    sent.erase("version");
    if (sent != nlohmann::json::parse(shape->data, nullptr, false))
        return "item " + key + " is not what the plugin's change holds";
    return {};
}

// PATCH /items/<key> of `collections` (one key in or out) or `deleted` (the trash), for a pending
// plugin change of the item; against its version.
std::string pluginPatchRefusal(const HttpRequest& req, const std::string& key, const nlohmann::json& body,
                               const WriteGuard::PluginChanges& pluginChanges) {
    using K = PluginChange::Kind;
    if (!pluginChanges) return "PATCH item " + key + " changes more than its tags";
    const auto shape = pluginChanges(key);
    if (!shape || !shape->item) return "item " + key + ": no plugin change pending";
    if (req.method != "PATCH") return "item " + key + ": only PATCH";
    if (!queryOf(req.url).empty()) return "PATCH item with a query";
    if (!versioned(req)) return "PATCH item " + key + " without If-Unmodified-Since-Version";
    if (body.contains("deleted")) {
        if (body["deleted"] != 1) return "PATCH item " + key + ": deleted is not 1";
        if (!pendingOf(*shape, K::Trash)) return "item " + key + " is not being moved to the trash";
        return {};
    }
    const nlohmann::json& list = body["collections"];
    if (!list.is_array()) return "PATCH item " + key + ": collections is not a list";
    std::set<std::string> after;
    for (const auto& c : list) {
        if (!isKey(c)) return "PATCH item " + key + " has a malformed collection";
        if (!after.insert(c.get<std::string>()).second) return "PATCH item " + key + " names a collection twice";
    }
    const std::set<std::string> before(shape->collections.begin(), shape->collections.end());
    std::vector<std::string> changed;
    std::set_symmetric_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(changed));
    if (changed.size() != 1)
        return "PATCH item " + key + " changes " + std::to_string(changed.size()) + " collections, not one";
    const std::string& coll = changed[0];
    const bool added = after.count(coll) > 0;
    if (!pendingOf(*shape, added ? K::AddToCollection : K::RemoveFromCollection,
                   [&](const PluginChange& c) { return c.value == coll; }))
        return "PATCH item " + key + ": no plugin change " + (added ? "adds it to " : "takes it out of ") + coll;
    return {};
}

// DELETE /items/<key> of a note: only one the plugin deleting it added; against its version.
std::string pluginDeleteRefusal(const HttpRequest& req, const std::string& key,
                                const WriteGuard::PluginChanges& pluginChanges) {
    if (!pluginChanges) return "item " + key + " is not an annotation";
    const auto shape = pluginChanges(key);
    if (!shape || !shape->note) return "item " + key + " is not an annotation or a note a plugin is deleting";
    if (shape->createdBy.empty()) return "note " + key + " was not added by a plugin";
    if (!pendingOf(*shape, PluginChange::Kind::DeleteNote,
                   [&](const PluginChange& c) { return c.pluginId == shape->createdBy; }))
        return "note " + key + " is not being deleted by the plugin that added it";
    if (!versioned(req)) return "DELETE note " + key + " without If-Unmodified-Since-Version";
    return {};
}

// Why the request must not go out, or empty if it may.
std::string refusal(const HttpRequest& req, const IsAnnotation& isAnnotation, const IsAnnotation& isAttachment,
                    const WriteGuard::ItemTags& itemTags, const WriteGuard::PluginChanges& pluginChanges) {
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
            return isAnnotation(match[1].str()) ? std::string{} : pluginDeleteRefusal(req, match[1].str(), pluginChanges);
        if (std::regex_match(path, itemsPath)) return bulkDeleteRefusal(query, isAnnotation);
        return "DELETE " + path + " is not an allowed write";
    }
    if ((m == "PUT" || m == "PATCH") && std::regex_match(path, match, itemPath)) {
        const std::string key = match[1].str();
        if (isAnnotation(key)) return {};
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_object() && body.size() == 1 && (body.contains("collections") || body.contains("deleted")))
            return pluginPatchRefusal(req, key, body, pluginChanges);
        return tagsRefusal(req, key, itemTags);
    }
    static const std::regex settingsPath(R"(^/users/\d+/settings/?$)");
    if (m == "POST" && std::regex_match(path, settingsPath)) return settingsRefusal(req, isAttachment);
    if (m == "POST" && std::regex_match(path, itemsPath)) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (!body.is_array()) return "POST body is not a JSON array";
        for (const auto& obj : body) {
            if (!obj.is_object()) return "POST body holds a non-object";
            auto type = obj.find("itemType");
            if (type == obj.end() || *type != "annotation") {
                if (!versioned(req)) return "POST of a plugin's object without If-Unmodified-Since-Version";
                if (std::string why = pluginPostRefusal(obj, pluginChanges); !why.empty()) return why;
                continue;
            }
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
                       std::function<bool(const std::string& key)> isAttachment, ItemTags itemTags,
                       PluginChanges pluginChanges)
    : inner_(inner), isAnnotation_(std::move(isAnnotation)), isAttachment_(std::move(isAttachment)),
      itemTags_(std::move(itemTags)), pluginChanges_(std::move(pluginChanges)) {}

HttpResponse WriteGuard::send(const HttpRequest& req) {
    const std::string why = refusal(req, isAnnotation_, isAttachment_, itemTags_, pluginChanges_);
    if (why.empty()) return inner_.send(req);
    log(LogLevel::Warn, "write guard refused %s %s: %s", req.method.c_str(), pathOf(req.url).c_str(), why.c_str());
    HttpResponse r;
    r.curlCode = -1;
    r.curlError = "blocked by write guard: " + why;
    return r;
}

}  // namespace zk
