#include "catalog/registry.h"

#include "plugins/manifest.h"
#include "update/signature.h"

#include <nlohmann/json.hpp>

#include <regex>
#include <set>

namespace zk::catalog {

namespace {

using nlohmann::json;

struct Bad {
    std::string why;
};

[[noreturn]] void bad(std::string why) { throw Bad{std::move(why)}; }

std::string str(const json& o, const char* key, bool required) {
    if (!o.contains(key)) {
        if (required) bad(std::string("missing \"") + key + "\"");
        return "";
    }
    if (!o[key].is_string()) bad(std::string("\"") + key + "\" must be a string");
    return o[key].get<std::string>();
}

bool validRepo(const std::string& repo) {
    static const std::regex re("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})/[A-Za-z0-9._-]{1,100}$");
    if (repo.size() > 140 || !std::regex_match(repo, re)) return false;
    const std::string name = repo.substr(repo.find('/') + 1);
    return name != "." && name != "..";
}

}  // namespace

std::optional<Registry> parseRegistry(std::string_view text, std::string* why) {
    try {
        if (text.size() > kMaxRegistryBytes) bad("over 1 MB");
        const json j = json::parse(text, nullptr, false);
        if (j.is_discarded()) bad("not JSON");
        if (!j.is_object()) bad("not an object");
        if (!j.contains("registry") || !j["registry"].is_number_integer() || j["registry"].get<int64_t>() != 1)
            bad("\"registry\" must be 1");
        Registry r;
        r.updated = str(j, "updated", false);
        if (!j.contains("plugins") || !j["plugins"].is_array()) bad("\"plugins\" must be an array");
        std::set<std::string> ids;
        for (const auto& p : j["plugins"]) {
            if (!p.is_object()) bad("a plugin entry is not an object");
            Entry e;
            e.id = str(p, "id", true);
            if (!validPluginId(e.id)) bad("bad id: " + e.id);
            if (!ids.insert(e.id).second) bad("duplicate id: " + e.id);
            e.name = str(p, "name", true);
            e.author = str(p, "author", false);
            e.description = str(p, "description", false);
            e.repo = str(p, "repo", true);
            if (!validRepo(e.repo)) bad("\"repo\" must be owner/name: " + e.repo);
            const auto key = base64Decode(str(p, "publicKey", true));
            if (!key || key->size() != 32) bad("\"publicKey\" must be 32 bytes in base64: " + e.id);
            std::copy(key->begin(), key->end(), e.publicKey.begin());
            r.plugins.push_back(std::move(e));
        }
        if (j.contains("revoked")) {
            if (!j["revoked"].is_array()) bad("\"revoked\" must be an array");
            for (const auto& x : j["revoked"]) {
                if (!x.is_object()) bad("a revoked entry is not an object");
                Revoked v{str(x, "id", true), str(x, "reason", true)};
                if (!validPluginId(v.id)) bad("bad revoked id: " + v.id);
                r.revoked.push_back(std::move(v));
            }
        }
        return r;
    } catch (const Bad& b) {
        if (why) *why = b.why;
    } catch (const std::exception& e) {
        if (why) *why = e.what();
    }
    return std::nullopt;
}

std::optional<Version> versionFromTag(std::string_view tag) { return parseVersion(tag); }

}  // namespace zk::catalog
