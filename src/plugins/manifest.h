#pragma once
// Plugin manifests (plugin.json), discovery of plugin folders, and what a permission means to the
// user. The parser accepts exactly what sdk/types/plugin-manifest.schema.json accepts.

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "net/errors.h"
#include "update/version.h"

namespace zk {

enum class Permission {
    LibraryRead, AnnotationsRead, NotesRead, FulltextRead, Net, NetAny,
    CreateItems, Collections, Tags, Notes, Remove, Background
};

struct PluginManifest {
    std::string id, name, version, author, description, main;
    int api = 0;
    Version minApp;
    std::set<Permission> permissions;
    std::vector<std::string> hosts;  // from net:<host> permissions, as written
};

// The id rule: lowercase letters, digits and single dashes between them, at most 64.
bool validPluginId(const std::string& id);

// nullopt and *why filled (if given) when the text is not a valid manifest.
std::optional<PluginManifest> parseManifest(const std::string& json, UserError* why);

// `s` on one line: every run of whitespace or control characters (line breaks, tabs, Unicode
// spaces and separators, C0/C1 controls) becomes one space, none at the ends; invisible format
// characters (soft hyphen, zero-width, bidi marks, embeddings and isolates, tag characters, BOM)
// are dropped; malformed UTF-8 becomes U+FFFD. For text a plugin puts in the app's own UI (its
// name, titles and tags in a confirmation).
std::string oneLine(const std::string& s);

struct PluginFolder {
    std::string dir;  // the folder's name (its path is <pluginsDir>/<dir>)
    std::optional<PluginManifest> manifest;
    std::string problem;  // "" if loadable
    // The problem: it needs a newer app (manifest->minApp), or was written for another plugin API
    // (otherApi, its manifest otherwise good and kept for its name; never loaded).
    bool incompatible = false;
    long long otherApi = 0;
};

// Every plugin folder in pluginsDir, sorted by name. Names starting with "." and plain files are
// not plugins and are skipped. A broken folder is listed with its problem; it hides no other.
std::vector<PluginFolder> scanPlugins(const std::string& pluginsDir, Version app);

// The wording shown to the user before a plugin is enabled.
std::string permissionText(Permission, const std::string& host = "");

// May the plugin fetch from this host? Exact match, or "*.example.org" for any subdomain.
bool hostAllowed(const PluginManifest&, const std::string& host);

}  // namespace zk
