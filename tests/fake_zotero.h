#pragma once
// An in-memory Zotero Web API v3 server for one user library (id 1), enough for sync tests:
// version listings, keyed fetches, /deleted, and POST /items with If-Unmodified-Since-Version.

#include "net/http.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
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
    int created = 0;

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

        if (req.method == "GET" && q.count("format") && q["format"] == "versions") {
            const auto& objs = path == "items" ? items : collections;
            const int64_t since = std::stoll(q["since"]);
            const std::string type = q.count("itemType") ? q["itemType"] : "";  // "annotation" or "-annotation"
            json out = json::object();
            for (const auto& [k, o] : objs) {
                if (o["version"].get<int64_t>() <= since) continue;
                const bool isAnnotation = o["data"].value("itemType", "") == "annotation";
                if ((type == "annotation" && !isAnnotation) || (type == "-annotation" && isAnnotation)) continue;
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
            return ok(r, out);
        }
        if (req.method == "POST" && path == "items") {
            int64_t ius = -1;
            for (const auto& [k, v] : req.headers)
                if (k == "If-Unmodified-Since-Version") ius = std::stoll(v);
            if (ius != libraryVersion) return status(r, 412);
            json body = json::parse(req.body);
            json success = json::object(), successful = json::object();
            ++libraryVersion;
            for (size_t i = 0; i < body.size(); ++i) {
                json data = body[i];
                std::string key = data.value("key", "");
                if (key.empty()) key = "SRV" + std::to_string(10000 + ++created);
                data["key"] = key;
                data["version"] = libraryVersion;
                items[key] = json{{"key", key}, {"version", libraryVersion}, {"data", data}};
                success[std::to_string(i)] = key;
                successful[std::to_string(i)] = items[key];
            }
            r.headers["last-modified-version"] = std::to_string(libraryVersion);
            return ok(r, json{{"success", success}, {"successful", successful}, {"unchanged", json::object()},
                              {"failed", json::object()}});
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
