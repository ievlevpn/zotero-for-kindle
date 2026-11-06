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
    std::string json;
};

struct CollectionRecord {
    std::string key, name, parentKey;
    int64_t version = 0;
};

}  // namespace zk
