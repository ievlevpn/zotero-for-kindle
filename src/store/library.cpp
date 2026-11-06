#include "store/library.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace zk {

using nlohmann::json;

namespace {

constexpr int64_t kSchemaVersion = 1;

// Schema version 1 (PRAGMA user_version). Text-heavy columns default to '' so reads never see NULL.
const char* kSchema = R"sql(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE items (
  key TEXT PRIMARY KEY, version INTEGER NOT NULL DEFAULT 0, itemType TEXT NOT NULL DEFAULT '',
  title TEXT NOT NULL DEFAULT '', date TEXT NOT NULL DEFAULT '', publication TEXT NOT NULL DEFAULT '',
  parentKey TEXT NOT NULL DEFAULT '', creators TEXT NOT NULL DEFAULT '[]', collections TEXT NOT NULL DEFAULT '[]',
  tags TEXT NOT NULL DEFAULT '[]', dateAdded TEXT NOT NULL DEFAULT '', dateModified TEXT NOT NULL DEFAULT '',
  json TEXT NOT NULL DEFAULT '{}', creatorsText TEXT NOT NULL DEFAULT '', tagsText TEXT NOT NULL DEFAULT '');
CREATE INDEX items_recent ON items(parentKey, dateAdded DESC);
CREATE TABLE item_collections (itemKey TEXT NOT NULL, collectionKey TEXT NOT NULL, PRIMARY KEY (collectionKey, itemKey));
CREATE INDEX item_collections_item ON item_collections(itemKey);
CREATE TABLE collections (key TEXT PRIMARY KEY, name TEXT NOT NULL DEFAULT '', parentKey TEXT NOT NULL DEFAULT '', version INTEGER NOT NULL DEFAULT 0);
CREATE TABLE attachments (
  key TEXT PRIMARY KEY, parentKey TEXT NOT NULL DEFAULT '', contentType TEXT NOT NULL DEFAULT '',
  filename TEXT NOT NULL DEFAULT '', md5 TEXT NOT NULL DEFAULT '', linkMode TEXT NOT NULL DEFAULT '',
  mtime INTEGER NOT NULL DEFAULT 0, localPath TEXT NOT NULL DEFAULT '', state INTEGER NOT NULL DEFAULT 0);
CREATE INDEX attachments_parent ON attachments(parentKey);
CREATE TABLE annotations (
  key TEXT PRIMARY KEY, parentKey TEXT NOT NULL DEFAULT '', type TEXT NOT NULL DEFAULT '', text TEXT NOT NULL DEFAULT '',
  comment TEXT NOT NULL DEFAULT '', color TEXT NOT NULL DEFAULT '', pageLabel TEXT NOT NULL DEFAULT '',
  sortIndex TEXT NOT NULL DEFAULT '', position TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '[]',
  version INTEGER NOT NULL DEFAULT 0, dirty INTEGER NOT NULL DEFAULT 0, conflict INTEGER NOT NULL DEFAULT 0,
  createdHere INTEGER NOT NULL DEFAULT 0, json TEXT NOT NULL DEFAULT '{}');
CREATE INDEX annotations_parent ON annotations(parentKey, sortIndex);
CREATE INDEX annotations_dirty ON annotations(dirty) WHERE dirty = 1;
CREATE TABLE object_versions (key TEXT PRIMARY KEY, version INTEGER NOT NULL);
CREATE VIRTUAL TABLE item_fts USING fts5(title, creators, publication, tags, notes,
  tokenize='unicode61 remove_diacritics 2');
)sql";

const char* kItemColumns =
    "items.key, items.version, items.itemType, items.title, items.date, items.publication, items.parentKey, "
    "items.creators, items.collections, items.tags, items.dateAdded, items.dateModified, items.json";

std::vector<std::string> strings(const std::string& text) {
    auto j = json::parse(text, nullptr, false);
    std::vector<std::string> out;
    if (j.is_array())
        for (const auto& v : j)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
    return s;
}

ItemRecord readItem(const Stmt& st) {
    ItemRecord r;
    r.key = st.text(0);
    r.version = st.integer(1);
    r.itemType = st.text(2);
    r.title = st.text(3);
    r.date = st.text(4);
    r.publication = st.text(5);
    r.parentKey = st.text(6);
    auto creators = json::parse(st.text(7), nullptr, false);
    if (creators.is_array())
        for (const auto& c : creators)
            r.creators.push_back({c.value("first", ""), c.value("last", ""), c.value("name", ""), c.value("type", "")});
    r.collections = strings(st.text(8));
    r.tags = strings(st.text(9));
    r.dateAdded = st.text(10);
    r.dateModified = st.text(11);
    r.json = st.text(12);
    return r;
}

std::vector<ItemRecord> readItems(Stmt& st) {
    std::vector<ItemRecord> out;
    while (st.step()) out.push_back(readItem(st));
    return out;
}

const char* kAnnotationColumns =
    "key, parentKey, type, text, comment, color, pageLabel, sortIndex, position, tags, version, dirty, conflict, "
    "createdHere, json";

std::vector<AnnotationRecord> readAnnotations(Stmt& st) {
    std::vector<AnnotationRecord> out;
    while (st.step()) {
        AnnotationRecord a;
        a.key = st.text(0);
        a.parentKey = st.text(1);
        a.type = st.text(2);
        a.text = st.text(3);
        a.comment = st.text(4);
        a.color = st.text(5);
        a.pageLabel = st.text(6);
        a.sortIndex = st.text(7);
        a.position = st.text(8);
        a.tags = strings(st.text(9));
        a.version = st.integer(10);
        a.dirty = st.integer(11) != 0;
        a.conflict = st.integer(12) != 0;
        a.createdHere = st.integer(13) != 0;
        a.json = st.text(14);
        out.push_back(std::move(a));
    }
    return out;
}

const char* kAttachmentColumns = "key, parentKey, contentType, filename, md5, linkMode, mtime, localPath, state";

std::vector<AttachmentRecord> readAttachments(Stmt& st) {
    std::vector<AttachmentRecord> out;
    while (st.step()) {
        AttachmentRecord a;
        a.key = st.text(0);
        a.parentKey = st.text(1);
        a.contentType = st.text(2);
        a.filename = st.text(3);
        a.md5 = st.text(4);
        a.linkMode = st.text(5);
        a.mtime = st.integer(6);
        a.localPath = st.text(7);
        a.state = static_cast<AttachmentRecord::State>(st.integer(8));
        out.push_back(std::move(a));
    }
    return out;
}

// User text -> an FTS5 query where every word is a quoted prefix term: `neur net` -> `"neur"* "net"*`.
// ASCII punctuation separates words, so nothing the user types can break the query syntax.
std::string ftsQuery(const std::string& input) {
    std::string out, word;
    auto flush = [&] {
        if (!word.empty()) out += (out.empty() ? "\"" : " \"") + word + "\"*";
        word.clear();
    };
    for (unsigned char c : input) {
        if (c < 0x80 && !std::isalnum(c)) flush();
        else word += static_cast<char>(c);
    }
    flush();
    return out;
}

}  // namespace

Transaction::Transaction(Library& lib) : lib_(lib) {
    lib_.guarded([&] { lib_.db_.exec("BEGIN IMMEDIATE"); });
    lib_.inTx_ = true;
}

Transaction::~Transaction() {
    if (done_) return;
    lib_.inTx_ = false;
    lib_.pending_.clear();
    if (!sqlite3_get_autocommit(lib_.db_.raw())) {
        try {
            lib_.db_.exec("ROLLBACK");
        } catch (const DbError&) {
        }
    }
}

void Transaction::commit() {
    lib_.guarded([&] {
        lib_.inTx_ = false;
        for (const auto& key : lib_.pending_) lib_.reindex(key);
        lib_.pending_.clear();
        lib_.db_.exec("COMMIT");
    });
    done_ = true;
}

Library::Library(Db& db) : db_(db) { guarded([&] { migrate(); }); }

template <class F> auto Library::guarded(F&& f) -> decltype(f()) {
    try {
        return f();
    } catch (const DbError& e) {
        if (e.diskFull())
            lastError_ = {ErrorKind::DiskFull, "Storage is full. Free some space on the Kindle, then sync again.", e.what()};
        else
            lastError_ = {ErrorKind::Other, "The library database failed (" + db_.path() + ").", e.what()};
        throw;
    }
}

void Library::migrate() {
    if (db_.userVersion() >= kSchemaVersion) return;
    db_.exec("BEGIN");
    try {
        db_.exec(kSchema);
        db_.setUserVersion(kSchemaVersion);
        db_.exec("COMMIT");
    } catch (...) {
        db_.exec("ROLLBACK");
        throw;
    }
}

Transaction Library::begin() { return Transaction(*this); }

void Library::touch(const std::string& itemKey) {
    if (itemKey.empty()) return;
    if (inTx_) pending_.insert(itemKey);
    else reindex(itemKey);
}

void Library::reindex(const std::string& itemKey) {
    Stmt del = db_.prepare("DELETE FROM item_fts WHERE rowid = (SELECT rowid FROM items WHERE key = ?)");
    del.bind(1, itemKey).step();
    Stmt ins = db_.prepare(
        "INSERT INTO item_fts(rowid, title, creators, publication, tags, notes) "
        "SELECT i.rowid, i.title, i.creatorsText, i.publication, i.tagsText, "
        "  (SELECT coalesce(group_concat(n.text || ' ' || n.comment || ' ' || n.tags, ' '), '') "
        "   FROM annotations n JOIN attachments a ON n.parentKey = a.key WHERE a.parentKey = i.key) "
        "FROM items i WHERE i.key = ?");
    ins.bind(1, itemKey).step();
}

std::string Library::itemOfAttachment(const std::string& attachmentKey) {
    Stmt st = db_.prepare("SELECT parentKey FROM attachments WHERE key = ?");
    st.bind(1, attachmentKey);
    return st.step() ? st.text(0) : std::string{};
}

void Library::upsertItem(const ItemRecord& r) {
    guarded([&] {
        json creators = json::array();
        std::string creatorsText;
        for (const auto& c : r.creators) {
            creators.push_back({{"first", c.first}, {"last", c.last}, {"name", c.name}, {"type", c.type}});
            for (const auto* part : {&c.first, &c.last, &c.name})
                if (!part->empty()) creatorsText += (creatorsText.empty() ? "" : " ") + *part;
        }
        Stmt st = db_.prepare(
            "INSERT INTO items (key, version, itemType, title, date, publication, parentKey, creators, collections, "
            "tags, dateAdded, dateModified, json, creatorsText, tagsText) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(key) DO UPDATE SET version=excluded.version, itemType=excluded.itemType, "
            "title=excluded.title, date=excluded.date, publication=excluded.publication, parentKey=excluded.parentKey, "
            "creators=excluded.creators, collections=excluded.collections, tags=excluded.tags, "
            "dateAdded=excluded.dateAdded, dateModified=excluded.dateModified, json=excluded.json, "
            "creatorsText=excluded.creatorsText, tagsText=excluded.tagsText");
        st.bind(1, r.key).bind(2, r.version).bind(3, r.itemType).bind(4, r.title).bind(5, r.date);
        st.bind(6, r.publication).bind(7, r.parentKey).bind(8, creators.dump());
        st.bind(9, json(r.collections).dump()).bind(10, json(r.tags).dump()).bind(11, r.dateAdded);
        st.bind(12, r.dateModified).bind(13, r.json).bind(14, creatorsText).bind(15, joined(r.tags));
        st.step();
        Stmt clear = db_.prepare("DELETE FROM item_collections WHERE itemKey = ?");
        clear.bind(1, r.key).step();
        for (const auto& c : r.collections) {
            Stmt add = db_.prepare("INSERT OR IGNORE INTO item_collections (itemKey, collectionKey) VALUES (?, ?)");
            add.bind(1, r.key).bind(2, c).step();
        }
        touch(r.key);
    });
}

void Library::upsertAttachment(const AttachmentRecord& r) {
    guarded([&] {
        const std::string oldParent = itemOfAttachment(r.key);
        Stmt st = db_.prepare(
            "INSERT INTO attachments (key, parentKey, contentType, filename, md5, linkMode, mtime, localPath, state) "
            "VALUES (?,?,?,?,?,?,?,?,?) ON CONFLICT(key) DO UPDATE SET parentKey=excluded.parentKey, "
            "contentType=excluded.contentType, filename=excluded.filename, md5=excluded.md5, "
            "linkMode=excluded.linkMode, mtime=excluded.mtime, localPath=excluded.localPath, state=excluded.state");
        st.bind(1, r.key).bind(2, r.parentKey).bind(3, r.contentType).bind(4, r.filename).bind(5, r.md5);
        st.bind(6, r.linkMode).bind(7, r.mtime).bind(8, r.localPath).bind(9, static_cast<int64_t>(r.state));
        st.step();
        if (oldParent != r.parentKey) touch(oldParent);
        touch(r.parentKey);
    });
}

void Library::upsertAnnotation(const AnnotationRecord& r) {
    guarded([&] {
        Stmt old = db_.prepare("SELECT parentKey FROM annotations WHERE key = ?");
        old.bind(1, r.key);
        const std::string oldAttachment = old.step() ? old.text(0) : std::string{};
        Stmt st = db_.prepare(
            std::string("INSERT INTO annotations (") + kAnnotationColumns +
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(key) DO UPDATE SET parentKey=excluded.parentKey, "
            "type=excluded.type, text=excluded.text, comment=excluded.comment, color=excluded.color, "
            "pageLabel=excluded.pageLabel, sortIndex=excluded.sortIndex, position=excluded.position, "
            "tags=excluded.tags, version=excluded.version, dirty=excluded.dirty, conflict=excluded.conflict, "
            "createdHere=excluded.createdHere, json=excluded.json");
        st.bind(1, r.key).bind(2, r.parentKey).bind(3, r.type).bind(4, r.text).bind(5, r.comment);
        st.bind(6, r.color).bind(7, r.pageLabel).bind(8, r.sortIndex).bind(9, r.position);
        st.bind(10, json(r.tags).dump()).bind(11, r.version).bind(12, int64_t{r.dirty});
        st.bind(13, int64_t{r.conflict}).bind(14, int64_t{r.createdHere}).bind(15, r.json.empty() ? "{}" : r.json);
        st.step();
        if (!oldAttachment.empty() && oldAttachment != r.parentKey) touch(itemOfAttachment(oldAttachment));
        touch(itemOfAttachment(r.parentKey));
    });
}

void Library::upsertCollection(const CollectionRecord& r) {
    guarded([&] {
        Stmt st = db_.prepare(
            "INSERT INTO collections (key, name, parentKey, version) VALUES (?,?,?,?) ON CONFLICT(key) DO UPDATE SET "
            "name=excluded.name, parentKey=excluded.parentKey, version=excluded.version");
        st.bind(1, r.key).bind(2, r.name).bind(3, r.parentKey).bind(4, r.version).step();
    });
}

void Library::removeObject(const std::string& type, const std::string& key) {
    guarded([&] {
        Stmt ver = db_.prepare("DELETE FROM object_versions WHERE key = ?");
        ver.bind(1, key).step();
        if (type == "item") {
            pending_.erase(key);
            Stmt fts = db_.prepare("DELETE FROM item_fts WHERE rowid = (SELECT rowid FROM items WHERE key = ?)");
            fts.bind(1, key).step();
            Stmt links = db_.prepare("DELETE FROM item_collections WHERE itemKey = ?");
            links.bind(1, key).step();
            Stmt st = db_.prepare("DELETE FROM items WHERE key = ?");
            st.bind(1, key).step();
        } else if (type == "attachment") {
            const std::string parent = itemOfAttachment(key);
            Stmt st = db_.prepare("DELETE FROM attachments WHERE key = ?");
            st.bind(1, key).step();
            touch(parent);
        } else if (type == "annotation") {
            Stmt old = db_.prepare("SELECT parentKey FROM annotations WHERE key = ?");
            old.bind(1, key);
            const std::string attachment = old.step() ? old.text(0) : std::string{};
            Stmt st = db_.prepare("DELETE FROM annotations WHERE key = ?");
            st.bind(1, key).step();
            touch(itemOfAttachment(attachment));
        } else if (type == "collection") {
            Stmt links = db_.prepare("DELETE FROM item_collections WHERE collectionKey = ?");
            links.bind(1, key).step();
            Stmt st = db_.prepare("DELETE FROM collections WHERE key = ?");
            st.bind(1, key).step();
        }
    });
}

int64_t Library::libraryVersion() {
    return guarded([&] {
        Stmt st = db_.prepare("SELECT value FROM meta WHERE key = 'libraryVersion'");
        return st.step() ? std::stoll(st.text(0)) : int64_t{0};
    });
}

void Library::setLibraryVersion(int64_t v) {
    guarded([&] {
        Stmt st = db_.prepare(
            "INSERT INTO meta (key, value) VALUES ('libraryVersion', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        st.bind(1, std::to_string(v)).step();
    });
}

std::vector<ItemRecord> Library::recentItems(int limit) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kItemColumns +
                              " FROM items WHERE parentKey = '' ORDER BY dateAdded DESC LIMIT ?");
        st.bind(1, int64_t{limit});
        return readItems(st);
    });
}

std::vector<ItemRecord> Library::itemsInCollection(const std::string& key) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kItemColumns +
                              " FROM item_collections c JOIN items ON items.key = c.itemKey WHERE c.collectionKey = ? "
                              "ORDER BY items.title COLLATE NOCASE");
        st.bind(1, key);
        return readItems(st);
    });
}

std::vector<CollectionRecord> Library::collections() {
    return guarded([&] {
        Stmt st = db_.prepare("SELECT key, name, parentKey, version FROM collections ORDER BY name COLLATE NOCASE");
        std::vector<CollectionRecord> out;
        while (st.step()) out.push_back({st.text(0), st.text(1), st.text(2), st.integer(3)});
        return out;
    });
}

std::vector<ItemRecord> Library::search(const std::string& query, int limit) {
    const std::string fts = ftsQuery(query);
    if (fts.empty()) return {};
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kItemColumns +
                              " FROM item_fts JOIN items ON items.rowid = item_fts.rowid WHERE item_fts MATCH ? "
                              "ORDER BY rank LIMIT ?");
        st.bind(1, fts).bind(2, int64_t{limit});
        return readItems(st);
    });
}

std::vector<AnnotationRecord> Library::annotationsFor(const std::string& attachmentKey) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAnnotationColumns +
                              " FROM annotations WHERE parentKey = ? ORDER BY sortIndex, key");
        st.bind(1, attachmentKey);
        return readAnnotations(st);
    });
}

std::vector<AnnotationRecord> Library::dirtyAnnotations() {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAnnotationColumns +
                              " FROM annotations WHERE dirty = 1 ORDER BY parentKey, sortIndex");
        return readAnnotations(st);
    });
}

std::vector<AttachmentRecord> Library::readableAttachments(const std::string& itemKey) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAttachmentColumns +
                              " FROM attachments WHERE parentKey = ? AND contentType IN ('application/epub+zip', "
                              "'application/pdf') AND linkMode NOT IN ('linked_file', 'linked_url') ORDER BY key");
        st.bind(1, itemKey);
        return readAttachments(st);
    });
}

std::optional<AttachmentRecord> Library::attachment(const std::string& key) {
    return guarded([&]() -> std::optional<AttachmentRecord> {
        Stmt st = db_.prepare(std::string("SELECT ") + kAttachmentColumns + " FROM attachments WHERE key = ?");
        st.bind(1, key);
        auto v = readAttachments(st);
        if (v.empty()) return std::nullopt;
        return v[0];
    });
}

std::optional<AnnotationRecord> Library::annotation(const std::string& key) {
    return guarded([&]() -> std::optional<AnnotationRecord> {
        Stmt st = db_.prepare(std::string("SELECT ") + kAnnotationColumns + " FROM annotations WHERE key = ?");
        st.bind(1, key);
        auto v = readAnnotations(st);
        if (v.empty()) return std::nullopt;
        return v[0];
    });
}

int64_t Library::objectVersion(const std::string& key) {
    return guarded([&] {
        Stmt st = db_.prepare("SELECT version FROM object_versions WHERE key = ?");
        st.bind(1, key);
        return st.step() ? st.integer(0) : int64_t{0};
    });
}

void Library::setObjectVersion(const std::string& key, int64_t version) {
    guarded([&] {
        Stmt st = db_.prepare(
            "INSERT INTO object_versions (key, version) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET version=excluded.version");
        st.bind(1, key).bind(2, version).step();
    });
}

}  // namespace zk
