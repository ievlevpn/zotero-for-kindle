#include "plugins/manifest.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

namespace zk {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

struct Bad {
    std::string msg;
};

[[noreturn]] void bad(std::string m) { throw Bad{std::move(m)}; }

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

const std::regex& idRe() { static const std::regex r("^[a-z0-9]+(-[a-z0-9]+)*$"); return r; }
const std::regex& semverRe() {
    static const std::regex r("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$");
    return r;
}
const std::regex& minAppRe() { static const std::regex r("^[0-9]+\\.[0-9]+\\.[0-9]+$"); return r; }
const std::regex& mainRe() { static const std::regex r("^[A-Za-z0-9._-]+\\.js$"); return r; }
const std::regex& hostRe() {
    static const std::regex r("^(\\*\\.)?[a-z0-9]([a-z0-9-]*[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$");
    return r;
}

std::string str(const json& j, const char* key, bool required) {
    auto it = j.find(key);
    if (it == j.end()) {
        if (required) bad(std::string("\"") + key + "\" is missing");
        return "";
    }
    if (!it->is_string()) bad(std::string("\"") + key + "\" must be text");
    return it->get<std::string>();
}

PluginManifest parseObject(const json& j) {
    if (!j.is_object()) bad("plugin.json must be an object");
    static const std::set<std::string> known = {"$schema", "id", "name", "version", "api", "minApp",
                                                "author", "description", "main", "permissions"};
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.count(it.key())) bad("unknown field \"" + it.key() + "\"");
    if (j.contains("$schema")) str(j, "$schema", true);

    PluginManifest m;
    m.id = str(j, "id", true);
    if (m.id.size() > 64 || !std::regex_match(m.id, idRe()))
        bad("\"id\" must be lowercase letters, digits and dashes (at most 64): " + m.id);
    m.name = str(j, "name", true);
    if (m.name.empty() || m.name.size() > 40) bad("\"name\" must be 1 to 40 characters");
    m.version = str(j, "version", true);
    if (!std::regex_match(m.version, semverRe())) bad("\"version\" must look like 1.2.3: " + m.version);
    m.main = str(j, "main", true);
    if (!std::regex_match(m.main, mainRe())) bad("\"main\" must be one .js file name: " + m.main);
    m.author = str(j, "author", false);
    m.description = str(j, "description", false);

    auto api = j.find("api");
    if (api == j.end()) bad("\"api\" is missing");
    if (!api->is_number_integer() || api->get<long long>() != 0)
        bad("it was written for plugin API " + api->dump() + "; this app runs API 0");
    m.api = 0;

    if (j.contains("minApp")) {
        std::string s = str(j, "minApp", true);
        auto v = std::regex_match(s, minAppRe()) ? parseVersion(s) : std::nullopt;
        if (!v) bad("\"minApp\" must look like 1.2.3: " + s);
        m.minApp = *v;
    }

    auto perms = j.find("permissions");
    if (perms == j.end()) bad("\"permissions\" is missing");
    if (!perms->is_array()) bad("\"permissions\" must be a list");
    static const std::vector<std::pair<std::string, Permission>> fixed = {
        {"library:read", Permission::LibraryRead},      {"annotations:read", Permission::AnnotationsRead},
        {"fulltext:read", Permission::FulltextRead},    {"notes:read", Permission::NotesRead},
        {"net:*", Permission::NetAny},                  {"zotero:create-items", Permission::CreateItems},
        {"zotero:collections", Permission::Collections}, {"zotero:tags", Permission::Tags},
        {"zotero:notes", Permission::Notes},            {"zotero:remove", Permission::Remove},
        {"background", Permission::Background}};
    std::set<std::string> seen;
    for (const auto& p : *perms) {
        if (!p.is_string()) bad("every permission must be text");
        std::string s = p.get<std::string>();
        if (!seen.insert(s).second) bad("permission listed twice: " + s);
        auto f = std::find_if(fixed.begin(), fixed.end(), [&](auto& e) { return e.first == s; });
        if (f != fixed.end()) {
            m.permissions.insert(f->second);
        } else if (s.rfind("net:", 0) == 0 && std::regex_match(s.substr(4), hostRe())) {
            m.permissions.insert(Permission::Net);
            m.hosts.push_back(s.substr(4));
        } else {
            bad("unknown permission \"" + s + "\"");
        }
    }
    return m;
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

std::optional<PluginManifest> parseManifest(const std::string& text, UserError* why) {
    try {
        json j = json::parse(text);
        return parseObject(j);
    } catch (const Bad& b) {
        if (why) *why = {ErrorKind::Other, "plugin.json: " + b.msg, ""};
    } catch (const json::exception& e) {
        if (why) *why = {ErrorKind::Other, "plugin.json is not valid JSON", e.what()};
    }
    return std::nullopt;
}

std::vector<PluginFolder> scanPlugins(const std::string& pluginsDir, Version app) {
    std::vector<PluginFolder> out;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(pluginsDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (!it->is_directory(ec)) continue;
        dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());

    std::set<std::string> ids;
    for (const auto& d : dirs) {
        PluginFolder f;
        f.dir = d.filename().string();
        std::string text;
        if (!readFile(d / "plugin.json", text)) {
            f.problem = "No plugin.json in this folder.";
        } else {
            UserError why;
            f.manifest = parseManifest(text, &why);
            if (!f.manifest) {
                f.problem = why.message;
            } else if (f.manifest->id != f.dir) {
                f.problem = "The id \"" + f.manifest->id + "\" in plugin.json must be the folder's name.";
            } else if (!ids.insert(f.manifest->id).second) {
                f.problem = "Another plugin already has the id \"" + f.manifest->id + "\".";
            } else if (app < f.manifest->minApp) {
                f.problem = "Needs app version " + f.manifest->minApp.str() + " or newer.";
            } else if (!fs::is_regular_file(d / f.manifest->main, ec)) {
                f.problem = "The file " + f.manifest->main + " is missing.";
            }
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::string permissionText(Permission p, const std::string& host) {
    switch (p) {
        case Permission::LibraryRead: return "Reads your library";
        case Permission::AnnotationsRead: return "Reads your highlights and notes";
        case Permission::FulltextRead: return "Reads the text of your books and papers";
        case Permission::NotesRead: return "Reads your notes";
        case Permission::Net: return "Connects to " + host;
        case Permission::NetAny: return "Connects to any website (it can send what it reads anywhere)";
        case Permission::CreateItems: return "Adds items to your library";
        case Permission::Collections: return "Files items into collections";
        case Permission::Tags: return "Adds tags";
        case Permission::Notes: return "Adds notes to items";
        case Permission::Remove:
            return "May ask to move items to the trash or remove tags and notes \xE2\x80\x94 each time with your yes";
        case Permission::Background: return "Works in the background (hourly)";
    }
    return "";
}

bool hostAllowed(const PluginManifest& m, const std::string& hostIn) {
    if (m.permissions.count(Permission::NetAny)) return true;
    const std::string host = lower(hostIn);
    if (host.empty()) return false;
    for (const auto& h : m.hosts) {
        std::string pat = lower(h);
        if (pat.rfind("*.", 0) == 0) {
            std::string suffix = pat.substr(1);  // ".example.org"
            if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
                return true;
        } else if (host == pat) {
            return true;
        }
    }
    return false;
}

}  // namespace zk
