#pragma once
// An in-memory Zotero Web API v3 server for one user library (id 1), enough for sync tests:
// version listings, keyed fetches, /deleted, POST /items, PATCH /items/<key>, DELETE /items/<key> and
// DELETE /items?itemKey= with If-Unmodified-Since-Version (and per-object `version` checks on POST).

#include "net/http.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace zk::test {

class FakeZotero : public Http {
public:
    using json = nlohmann::json;

    int64_t libraryVersion = 0;
    std::map<std::string, json> items;        // key -> full object {key, version, data}
    std::map<std::string, json> collections;  // key -> full object
    std::map<std::string, int64_t> deletedItems, deletedCollections;  // key -> version of deletion
    std::vector<HttpRequest> sent;
    std::function<bool(const HttpRequest&)> failWhen;  // true -> connection timeout
    bool loseNextWriteResponse = false;  // apply the next POST, then answer with a timeout
    int created = 0;
    std::map<std::string, int> failObjects;  // POST: these keys fail with this code; PATCH, DELETE of one: answered so
    std::map<std::string, json> settings;    // key -> {value, version}
    std::map<std::string, int64_t> deletedSettings;  // key -> version of deletion
    std::set<std::string> answerUnchanged;   // POST: written, but answered as "unchanged"
    bool omitSuccessful = false;             // POST: "success" without the "successful" copies

    // A setting changed elsewhere (the desktop's reader): bumps the library version.
    void putSetting(const std::string& key, json value) {
        settings[key] = json{{"value", std::move(value)}, {"version", ++libraryVersion}};
    }

    // --- library editing (each bumps the library version) ---
    void put(json data) {
        const std::string key = data["key"];
        data["version"] = ++libraryVersion;
        items[key] = json{{"key", key}, {"version", libraryVersion}, {"data", data}};
    }
    void putCollection(const std::string& key, const std::string& name) {
        ++libraryVersion;
        collections[key] = json{{"key", key}, {"version", libraryVersion},
                                {"data", {{"key", key}, {"name", name}, {"parentCollection", false}}}};
    }
    void remove(const std::string& key) {
        items.erase(key);
        deletedItems[key] = ++libraryVersion;
    }
    static json item(const std::string& key, const std::string& title, const std::string& dateAdded = "2024-01-01T00:00:00Z") {
        return {{"key", key}, {"itemType", "book"}, {"title", title}, {"creators", json::array()},
                {"tags", json::array()}, {"collections", json::array()}, {"dateAdded", dateAdded},
                {"dateModified", dateAdded}};
    }
    static json attachment(const std::string& key, const std::string& parent, const std::string& md5 = "00") {
        return {{"key", key}, {"itemType", "attachment"}, {"parentItem", parent}, {"linkMode", "imported_file"},
                {"contentType", "application/epub+zip"}, {"filename", "book.epub"}, {"md5", md5}, {"mtime", 1}};
    }
    static json standalone(const std::string& key, const std::string& title) {
        json a = attachment(key, "");
        a.erase("parentItem");
        a["title"] = title;
        a["contentType"] = "application/pdf";
        a["filename"] = "lonely.pdf";
        return a;
    }
    static json note(const std::string& key, const std::string& parent, const std::string& html) {
        json n = {{"key", key}, {"itemType", "note"}, {"note", html}, {"tags", json::array()},
                  {"dateModified", "2024-02-02T00:00:00Z"}};
        if (!parent.empty()) n["parentItem"] = parent;
        return n;
    }
    static json annotation(const std::string& key, const std::string& parent, const std::string& text) {
        return {{"key", key}, {"itemType", "annotation"}, {"parentItem", parent}, {"annotationType", "highlight"},
                {"annotationText", text}, {"annotationComment", ""}, {"annotationColor", "#ffd400"},
                {"annotationSortIndex", "00001|00000001"}, {"annotationPosition", "{}"}, {"tags", json::array()}};
    }

    // --- Http ---
    HttpResponse send(const HttpRequest& req) override {
        sent.push_back(req);
        HttpResponse r;
        if (failWhen && failWhen(req)) {
            r.curlCode = 28;
            r.curlError = "Operation timed out";
            return r;
        }
        const std::string prefix = "https://api.zotero.org/users/1/";
        if (req.url.rfind(prefix, 0) != 0) return status(r, 404);
        std::string rest = req.url.substr(prefix.size());
        std::string path = rest.substr(0, rest.find('?'));
        std::map<std::string, std::string> q = query(rest);
        r.headers["last-modified-version"] = std::to_string(libraryVersion);

        // /settings[?since=N]; POST /settings {key: {value, version}} (per-setting version check)
        if (req.method == "GET" && path == "settings") {
            const int64_t since = q.count("since") ? std::stoll(q["since"]) : 0;
            json out = json::object();
            for (const auto& [k, v] : settings)
                if (v["version"].get<int64_t>() > since) out[k] = v;
            return ok(r, out);
        }
        if (req.method == "POST" && path == "settings") {
            json body = json::parse(req.body);
            for (const auto& [k, v] : body.items()) {
                const int64_t have = settings.count(k) ? settings[k]["version"].get<int64_t>() : 0;
                if (v.contains("version") && v["version"].get<int64_t>() != have) return status(r, 412);
            }
            ++libraryVersion;
            for (const auto& [k, v] : body.items()) settings[k] = json{{"value", v["value"]}, {"version", libraryVersion}};
            r.headers["last-modified-version"] = std::to_string(libraryVersion);
            if (loseNextWriteResponse) {
                loseNextWriteResponse = false;
                HttpResponse lost;
                lost.curlCode = 28;
                lost.curlError = "Operation timed out";
                return lost;
            }
            return status(r, 204);
        }

        // /items/<parent>/children?format=versions[&since=N][&itemType=annotation]
        if (req.method == "GET" && path.rfind("items/", 0) == 0 && path.size() > 15 &&
            path.compare(path.size() - 9, 9, "/children") == 0) {
            const std::string parent = path.substr(6, path.size() - 15);
            const int64_t since = q.count("since") ? std::stoll(q["since"]) : 0;
            json out = json::object();
            for (const auto& [k, o] : items)
                if (o["data"].value("parentItem", "") == parent && o["version"].get<int64_t>() > since &&
                    (!q.count("itemType") || o["data"].value("itemType", "") == q["itemType"]))
                    out[k] = o["version"];
            return ok(r, out);
        }
        if (req.method == "GET" && q.count("format") && q["format"] == "versions") {
            const auto& objs = path == "items" ? items : collections;
            const int64_t since = std::stoll(q["since"]);
            const std::string type = q.count("itemType") ? q["itemType"] : "";  // "annotation" or "-annotation"
            json out = json::object();
            for (const auto& [k, o] : objs) {
                if (o["version"].get<int64_t>() <= since) continue;
                const bool isAnnotation = o["data"].value("itemType", "") == "annotation";
                if ((type == "annotation" && !isAnnotation) || (type == "-annotation" && isAnnotation)) continue;
                if (type == "note" && o["data"].value("itemType", "") != "note") continue;
                out[k] = o["version"];
            }
            return ok(r, out);
        }
        if (req.method == "GET" && (q.count("itemKey") || q.count("collectionKey"))) {
            const auto& objs = path == "items" ? items : collections;
            json out = json::array();
            for (const auto& k : split(q.count("itemKey") ? q["itemKey"] : q["collectionKey"]))
                if (auto it = objs.find(k); it != objs.end()) out.push_back(it->second);
            return ok(r, out);
        }
        if (req.method == "GET" && path == "deleted") {
            const int64_t since = std::stoll(q["since"]);
            json out = {{"items", json::array()}, {"collections", json::array()}, {"searches", json::array()},
                        {"tags", json::array()}, {"settings", json::array()}};
            for (const auto& [k, v] : deletedItems)
                if (v > since) out["items"].push_back(k);
            for (const auto& [k, v] : deletedCollections)
                if (v > since) out["collections"].push_back(k);
            for (const auto& [k, v] : deletedSettings)
                if (v > since) out["settings"].push_back(k);
            return ok(r, out);
        }
        if (req.method == "POST" && path == "items") {
            int64_t ius = -1;
            for (const auto& [k, v] : req.headers)
                if (k == "If-Unmodified-Since-Version") ius = std::stoll(v);
            if (ius != libraryVersion) return status(r, 412);
            json body = json::parse(req.body);
            json success = json::object(), successful = json::object(), failed = json::object(), unchanged = json::object();
            ++libraryVersion;
            for (size_t i = 0; i < body.size(); ++i) {
                json data = body[i];
                std::string key = data.value("key", "");
                if (failObjects.count(key)) {
                    failed[std::to_string(i)] = {{"key", key}, {"code", failObjects[key]}, {"message", "refused"}};
                    continue;
                }
                if (data.contains("version") && items.count(key) && items[key]["version"] != data["version"]) {
                    failed[std::to_string(i)] = {{"key", key}, {"code", 412}, {"message", "Item has been modified"}};
                    continue;
                }
                if (key.empty()) key = "SRV" + std::to_string(10000 + ++created);
                data["key"] = key;
                data["version"] = libraryVersion;
                items[key] = json{{"key", key}, {"version", libraryVersion}, {"data", data}};
                if (answerUnchanged.count(key)) {
                    unchanged[std::to_string(i)] = key;
                    continue;
                }
                success[std::to_string(i)] = key;
                if (!omitSuccessful) successful[std::to_string(i)] = items[key];
            }
            r.headers["last-modified-version"] = std::to_string(libraryVersion);
            if (loseNextWriteResponse) {
                loseNextWriteResponse = false;
                HttpResponse lost;
                lost.curlCode = 28;
                lost.curlError = "Operation timed out";
                return lost;
            }
            return ok(r, json{{"success", success}, {"successful", successful}, {"unchanged", unchanged},
                              {"failed", failed}});
        }
        // PATCH /items/<key>: the given fields, against the item's own version
        if (req.method == "PATCH" && path.rfind("items/", 0) == 0) {
            const std::string key = path.substr(6);
            if (!items.count(key)) return status(r, 404);
            if (failObjects.count(key)) {
                r.body = "refused";
                return status(r, failObjects[key]);
            }
            int64_t ius = -1;
            for (const auto& [k, v] : req.headers)
                if (k == "If-Unmodified-Since-Version") ius = std::stoll(v);
            if (ius != items[key]["version"].get<int64_t>()) return status(r, 412);
            json data = items[key]["data"];
            const json body = json::parse(req.body);
            for (const auto& [k, v] : body.items()) data[k] = v;
            put(data);
            r.headers["last-modified-version"] = std::to_string(libraryVersion);
            return status(r, 204);
        }
        // DELETE /items/<key>: against the object's own version
        if (req.method == "DELETE" && path.rfind("items/", 0) == 0) {
            const std::string key = path.substr(6);
            if (!items.count(key)) return status(r, 404);
            if (failObjects.count(key)) {
                r.body = "refused";
                return status(r, failObjects[key]);
            }
            int64_t ius = -1;
            for (const auto& [k, v] : req.headers)
                if (k == "If-Unmodified-Since-Version") ius = std::stoll(v);
            if (ius != items[key]["version"].get<int64_t>()) return status(r, 412);
            remove(key);
            r.headers["last-modified-version"] = std::to_string(libraryVersion);
            return status(r, 204);
        }
        if (req.method == "DELETE" && path == "items" && q.count("itemKey")) {
            int64_t ius = -1;
            for (const auto& [k, v] : req.headers)
                if (k == "If-Unmodified-Since-Version") ius = std::stoll(v);
            if (ius != libraryVersion) return status(r, 412);
            ++libraryVersion;
            for (const auto& k : split(q["itemKey"])) {
                items.erase(k);
                deletedItems[k] = libraryVersion;
            }
            r.headers["last-modified-version"] = std::to_string(libraryVersion);
            return status(r, 204);
        }
        return status(r, 400);
    }

    // Keys requested by item fetches, in order.
    std::vector<std::string> fetchedItemKeys() const {
        std::vector<std::string> out;
        for (const auto& s : sent) {
            auto q = query(s.url);
            if (q.count("itemKey"))
                for (const auto& k : split(q["itemKey"])) out.push_back(k);
        }
        return out;
    }
    int count(const std::string& method, const std::string& needle) const {
        int n = 0;
        for (const auto& s : sent) n += s.method == method && s.url.find(needle) != std::string::npos;
        return n;
    }

private:
    static HttpResponse& ok(HttpResponse& r, const json& body) {
        r.status = 200;
        r.body = body.dump();
        return r;
    }
    static HttpResponse& status(HttpResponse& r, long s) {
        r.status = s;
        return r;
    }
    static std::map<std::string, std::string> query(const std::string& url) {
        std::map<std::string, std::string> out;
        size_t qpos = url.find('?');
        if (qpos == std::string::npos) return out;
        std::string qs = url.substr(qpos + 1);
        size_t start = 0;
        while (start <= qs.size()) {
            size_t amp = qs.find('&', start);
            std::string part = qs.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            if (size_t eq = part.find('='); eq != std::string::npos) out[part.substr(0, eq)] = part.substr(eq + 1);
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
        return out;
    }
    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < s.size()) {
            size_t c = s.find(',', start);
            out.push_back(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
            if (c == std::string::npos) break;
            start = c + 1;
        }
        return out;
    }
};

}  // namespace zk::test
