#pragma once
// Zotero with group libraries, for sync tests: the user's library (FakeZotero, user 1) and any
// number of groups (each a FakeZotero of its own under /groups/<id>/), GET /keys/current,
// GET /users/1/groups?format=versions and GET /groups/<id>. Every request is recorded in `sent`.

#include "fake_zotero.h"

#include <map>
#include <memory>
#include <string>

namespace zk::test {

class FakeZoteroHub : public Http {
public:
    using json = nlohmann::json;

    FakeZotero user;
    struct Group {
        std::unique_ptr<FakeZotero> server = std::make_unique<FakeZotero>();
        std::string name;
        int64_t version = 1;  // its metadata version
        int64_t owner = 2;
        std::vector<int64_t> admins;
        std::string libraryEditing = "members", fileEditing = "members";
        bool listed = true;  // in the user's group listing (false: left, deleted, out of the key's reach)
    };
    std::map<int64_t, Group> groups;
    // keys/current: access.groups (default: every group, read and write).
    json groupAccess = json{{"all", {{"library", true}, {"write", true}}}};
    std::vector<HttpRequest> sent;

    FakeZotero& addGroup(int64_t id, const std::string& name, const std::string& editing = "members") {
        Group& g = groups[id];
        g.name = name;
        g.libraryEditing = g.fileEditing = editing;
        g.server->libraryPath = "groups/" + std::to_string(id) + "/";
        return *g.server;
    }
    FakeZotero& group(int64_t id) { return *groups.at(id).server; }
    int count(const std::string& needle) const {
        int n = 0;
        for (const auto& r : sent) n += r.url.find(needle) != std::string::npos;
        return n;
    }

    HttpResponse send(const HttpRequest& req) override {
        sent.push_back(req);
        const std::string base = "https://api.zotero.org/";
        std::string rest = req.url.rfind(base, 0) == 0 ? req.url.substr(base.size()) : req.url;
        HttpResponse r;
        if (req.method == "GET" && rest == "keys/current") {
            json access = {{"user", {{"library", true}, {"write", true}, {"files", true}}}};
            if (!groupAccess.is_null()) access["groups"] = groupAccess;
            return ok(r, json{{"key", "k3y"}, {"userID", 1}, {"username", "pavel"}, {"access", access}});
        }
        if (req.method == "GET" && rest == "users/1/groups?format=versions") {
            json out = json::object();
            for (const auto& [id, g] : groups)
                if (g.listed) out[std::to_string(id)] = g.version;
            return ok(r, out);
        }
        if (rest.rfind("groups/", 0) == 0) {
            const size_t slash = rest.find('/', 7);
            const int64_t id = std::stoll(rest.substr(7, slash == std::string::npos ? std::string::npos : slash - 7));
            auto it = groups.find(id);
            if (it == groups.end() || !it->second.listed) {
                r.status = 403;
                return r;
            }
            if (slash == std::string::npos) {
                const Group& g = it->second;
                json data = {{"id", id}, {"version", g.version}, {"name", g.name}, {"owner", g.owner},
                             {"libraryEditing", g.libraryEditing}, {"libraryReading", "all"}, {"fileEditing", g.fileEditing}};
                if (!g.admins.empty()) data["admins"] = g.admins;
                return ok(r, json{{"id", id}, {"version", g.version}, {"data", data}});
            }
            return it->second.server->send(req);
        }
        return user.send(req);
    }

private:
    static HttpResponse& ok(HttpResponse& r, const json& body) {
        r.status = 200;
        r.body = body.dump();
        return r;
    }
};

}  // namespace zk::test
