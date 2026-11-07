#pragma once
// Plain records shared by the store, the Zotero JSON parser and the sync engine.

#include <cstdint>
#include <string>
#include <vector>

namespace zk {

struct Creator {
    std::string first, last, name, type;  // `name` for single-field creators
};

struct ItemRecord {
    std::string key;
    int64_t version = 0;
    std::string itemType, title, date, publication, parentKey;
    std::vector<Creator> creators;
    std::vector<std::string> collections, tags;
    std::string dateAdded, dateModified;
    std::string json;  // raw `data` object, for round-trips
    std::string year;         // from meta.parsedDate (else the first 4-digit run of `date`)
    std::string sortCreator;  // first creator's last name or name; filled by the store if empty
};

struct AttachmentRecord {
    std::string key, parentKey, contentType, filename, md5, linkMode;
    int64_t mtime = 0;
    std::string localPath;
    enum class State { Remote, Downloading, Local } state = State::Remote;
};

struct AnnotationRecord {
    std::string key, parentKey /* attachment */, type, text, comment, color, pageLabel, sortIndex,
        position /* JSON string */;
    std::vector<std::string> tags;
    int64_t version = 0;
    bool dirty = false;
    bool conflict = false;
    bool createdHere = false;
    bool deletedHere = false;  // a tombstone: deleted on the Kindle, DELETE not sent yet; hidden everywhere
    bool localOnly = false;    // its position can't be synced exactly (failed round trip): never uploaded
    std::string conflictOf;    // a conflict copy: the annotation it was an edit of
    std::string json;
    std::string dateModified;
};

struct CollectionRecord {
    std::string key, name, parentKey;
    int64_t version = 0;
};

}  // namespace zk
