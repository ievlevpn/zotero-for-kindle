#pragma once
// The app writes annotations, reading positions and the read tag, and nothing else, enforced in
// one place. Every request to zotero.org or WebDAV passes through this guard: annotations may be
// created, edited and deleted (like in Zotero desktop), the reading-position settings of the
// library's own files (lastPageIndex_u_<attachmentKey>) may be set, and an item's tags may gain or
// lose one tag (a pending read mark), against the item's version and with no other field. A plugin's
// change (zk.zotero.*) goes out only in exactly the shape of a pending plugin change for that key,
// one narrow rule per kind (see PluginChangeShape). Every other write is refused and never reaches
// the network.
// Group libraries: the same rules hold under /groups/<id>/ as under /users/<id>/, for objects of that
// library only, and only for a group the Kindle may edit (setLibraries); without setLibraries, no
// group write goes out. Settings are the user's alone: /users/<id>/settings, where a group file's
// reading position is lastPageIndex_g<groupID>_<attachmentKey> (as Zotero names it).

#include "net/http.h"
#include "store/plugin_changes.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace zk {

// What the library knows of a key that plugin changes are pending on, for the guard's plugin rules:
//   - POST /items, an object {key, itemType, metadata fields, creators, collections, tags} (no
//     version, or 0): a pending CreateItem of that key, and exactly its data.
//   - POST /items, an object {key, itemType: note, parentItem, note}: a pending AddNote of that key,
//     its parent and its HTML.
//   - PATCH /items/<key> {"collections": [...]}, against a version: Zotero's collections of the item
//     with exactly one key added or taken out, as a pending AddToCollection / RemoveFromCollection.
//   - PATCH /items/<key> {"deleted": 1}, against a version: a pending Trash of the item.
//   - DELETE /items/<key>, against a version: a note the same plugin added, with its DeleteNote pending.
//   - PATCH /items/<key> {"tags": [...]}, against a version: Zotero's tags with exactly one tag
//     added or taken off, as a pending read mark (read: added) or AddTag / RemoveTag of it.
// Every PATCH and DELETE is against `version`, the object's version as the library knows it.
struct PluginChangeShape {
    std::vector<PluginChange> pending;     // the pending plugin changes whose key (Change.key) this is
    std::vector<std::pair<std::string, bool>> readMarks;  // pending read marks of the item: (tag, read)
    bool item = false, note = false;       // what the key is in the library (as Zotero has it)
    int64_t version = 0;                   // its version in Zotero, as last pulled or sent
    std::string data;                      // its Zotero data (JSON); a pending create's: as it is to be sent
    std::vector<std::string> collections;  // an item's collections as Zotero has them
    std::string createdBy;                 // a note: the plugin that added it ("" if none did)
};

class WriteGuard final : public Http {
public:
    using ItemTags = std::function<std::optional<std::vector<std::string>>(const std::string& key)>;
    // nullopt: no plugin change or read mark is pending on the key.
    using PluginChanges = std::function<std::optional<PluginChangeShape>(const std::string& key)>;
    // isAttachment: a file of the library (its reading position may be set); null = no settings.
    // itemTags: the tag names of a regular item of the library, nullopt for anything else (its
    // tags may change by one tag, as pluginChanges has pending); null = no item writes.
    // pluginChanges: what is pending on a key (see PluginChangeShape); null = no item writes.
    WriteGuard(Http& inner, std::function<bool(const std::string& key)> isAnnotation,
               std::function<bool(const std::string& key)> isAttachment = nullptr, ItemTags itemTags = nullptr,
               PluginChanges pluginChanges = nullptr);
    HttpResponse send(const HttpRequest&) override;
    // Which library holds a key (0 My Library, a group ID; nullopt: none), and whether a group may be
    // written to (its key access and editing rules). Unset: only My Library's objects, by the
    // callbacks above, and nothing under /groups/.
    using LibraryOf = std::function<std::optional<int64_t>(const std::string& key)>;
    void setLibraries(LibraryOf libraryOf, std::function<bool(int64_t groupId)> groupWritable) {
        libraryOf_ = std::move(libraryOf);
        groupWritable_ = std::move(groupWritable);
    }

private:
    Http& inner_;
    std::function<bool(const std::string&)> isAnnotation_, isAttachment_;
    ItemTags itemTags_;
    PluginChanges pluginChanges_;
    LibraryOf libraryOf_;
    std::function<bool(int64_t)> groupWritable_;
};

}  // namespace zk
