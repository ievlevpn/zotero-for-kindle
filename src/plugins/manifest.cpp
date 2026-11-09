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

// Characters (code points), as the schema's maxLength counts them.
size_t codePoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

// Lengths are checked before any regex runs, so a huge string never reaches the matcher.
bool matches(const std::string& s, size_t maxLen, const std::regex& re) {
    return s.size() <= maxLen && std::regex_match(s, re);
}

constexpr size_t kMaxManifestBytes = 64 * 1024;

PluginManifest parseObject(const json& j) {
    if (!j.is_object()) bad("plugin.json must be an object");
    static const std::set<std::string> known = {"$schema", "id", "name", "version", "api", "minApp",
                                                "author", "description", "main", "permissions"};
    for (auto it = j.begin(); it != j.end(); ++it)
        if (!known.count(it.key())) bad("unknown field \"" + it.key() + "\"");
    if (j.contains("$schema")) str(j, "$schema", true);

    PluginManifest m;
    m.id = str(j, "id", true);
    if (!matches(m.id, 64, idRe()))
        bad("\"id\" must be lowercase letters, digits and dashes (at most 64): " + m.id);
    m.name = oneLine(str(j, "name", true));
    if (m.name.empty() || codePoints(m.name) > 40) bad("\"name\" must be 1 to 40 characters");
    m.version = str(j, "version", true);
    if (!matches(m.version, 100, semverRe()))
        bad("\"version\" must look like 1.2.3 (at most 100 characters)");
    m.main = str(j, "main", true);
    if (!matches(m.main, 100, mainRe())) bad("\"main\" must be one .js file name (at most 100 characters)");
    m.author = str(j, "author", false);
    m.description = str(j, "description", false);

    auto api = j.find("api");
    if (api == j.end()) bad("\"api\" is missing");
    if (!api->is_number_integer() || api->get<long long>() != 0)
        bad("it was written for plugin API " + api->dump() + "; this app runs API 0");
    m.api = 0;

    if (j.contains("minApp")) {
        std::string s = str(j, "minApp", true);
        if (!matches(s, 40, minAppRe())) bad("\"minApp\" must look like 1.2.3");
        auto v = parseVersion(s);
        if (!v) bad("\"minApp\": version numbers are too large");
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
        if (s.size() > 300) bad("a permission is too long");
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

enum class Read { Ok, Missing, TooBig };

Read readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return Read::Missing;
    std::string buf(kMaxManifestBytes + 1, '\0');
    in.read(buf.data(), (std::streamsize)buf.size());
    buf.resize((size_t)in.gcount());
    if (buf.size() > kMaxManifestBytes) return Read::TooBig;
    out = std::move(buf);
    return Read::Ok;
}

}  // namespace

std::string oneLine(const std::string& s) {
    std::string out;
    bool gap = false;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t n = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
        if (i + n > s.size()) n = 1;
        uint32_t cp = b;
        if (n == 2) cp = ((b & 0x1Fu) << 6) | (s[i + 1] & 0x3F);
        if (n == 3) cp = ((b & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3F);
        if (n == 4) cp = ((b & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3F);
        const bool blank = cp <= 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0xA0) || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200F) ||
                           (cp >= 0x2028 && cp <= 0x202F) || cp == 0x205F || cp == 0x2060 || cp == 0x3000 || cp == 0xFEFF;
        if (blank) {
            gap = true;
        } else {
            if (gap && !out.empty()) out += ' ';
            gap = false;
            out.append(s, i, n);
        }
        i += n;
    }
    return out;
}

std::optional<PluginManifest> parseManifest(const std::string& text, UserError* why) {
    try {
        if (text.size() > kMaxManifestBytes) bad("plugin.json is larger than 64 KB");
        json j = json::parse(text);
        return parseObject(j);
    } catch (const Bad& b) {
        if (why) *why = {ErrorKind::Other, "plugin.json: " + b.msg, ""};
    } catch (const json::exception& e) {
        if (why) *why = {ErrorKind::Other, "plugin.json is not valid JSON", e.what()};
    } catch (const std::exception& e) {
        if (why) *why = {ErrorKind::Other, "plugin.json could not be read", e.what()};
    }
    return std::nullopt;
}

std::vector<PluginFolder> scanPlugins(const std::string& pluginsDir, Version app) {
    std::vector<PluginFolder> out;
    std::error_code ec;
    std::vector<fs::path> dirs;
    std::vector<std::pair<fs::path, bool>> entries;  // path, is a folder
    for (fs::directory_iterator it(pluginsDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code dirEc;
        bool isDir = it->is_directory(dirEc);
        if (dirEc) {  // e.g. a dangling symlink: a problem of its own, never the end of the scan
            entries.push_back({it->path(), false});
            continue;
        }
        if (isDir) entries.push_back({it->path(), true});
    }
    std::sort(entries.begin(), entries.end());

    std::set<std::string> ids;
    for (const auto& [d, isDir] : entries) {
        PluginFolder f;
        f.dir = d.filename().string();
        std::string text;
        Read r = isDir ? readFile(d / "plugin.json", text) : Read::Missing;
        if (r == Read::Missing) {
            f.problem = isDir ? "No plugin.json in this folder." : "This entry can't be read.";
        } else if (r == Read::TooBig) {
            f.problem = "plugin.json is larger than 64 KB";
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
            } else if (std::error_code mainEc; !fs::is_regular_file(d / f.manifest->main, mainEc)) {
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
