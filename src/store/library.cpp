#include "store/library.h"

#include "core/log.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace zk {

using nlohmann::json;

namespace {

constexpr int64_t kSchemaVersion = 2;

// Schema version 2 (PRAGMA user_version). Text-heavy columns default to '' so reads never see NULL.
// items.id is the stable rowid the FTS index points at (a VACUUM can't renumber it).
// readableKey/readableType: the item's file to open (EPUB preferred), kept by reindex().
const char* kSchema = R"sql(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE items (
  id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, version INTEGER NOT NULL DEFAULT 0, itemType TEXT NOT NULL DEFAULT '',
  title TEXT NOT NULL DEFAULT '', date TEXT NOT NULL DEFAULT '', publication TEXT NOT NULL DEFAULT '',
  parentKey TEXT NOT NULL DEFAULT '', creators TEXT NOT NULL DEFAULT '[]', collections TEXT NOT NULL DEFAULT '[]',
  tags TEXT NOT NULL DEFAULT '[]', dateAdded TEXT NOT NULL DEFAULT '', dateModified TEXT NOT NULL DEFAULT '',
  json TEXT NOT NULL DEFAULT '{}', creatorsText TEXT NOT NULL DEFAULT '', tagsText TEXT NOT NULL DEFAULT '',
  year TEXT NOT NULL DEFAULT '', sortCreator TEXT NOT NULL DEFAULT '', readableKey TEXT NOT NULL DEFAULT '',
  readableType TEXT NOT NULL DEFAULT '');
CREATE INDEX items_recent ON items(parentKey, dateAdded DESC);
CREATE INDEX items_title ON items(parentKey, title COLLATE NOCASE);
CREATE INDEX items_author ON items(parentKey, sortCreator COLLATE NOCASE, title COLLATE NOCASE);
CREATE INDEX items_year ON items(parentKey, year DESC, title COLLATE NOCASE);
CREATE INDEX items_readable ON items(parentKey, readableType);
CREATE TABLE reading_state (itemKey TEXT PRIMARY KEY, attachmentKey TEXT NOT NULL DEFAULT '',
  openedMs INTEGER NOT NULL DEFAULT 0, progress REAL NOT NULL DEFAULT 0, location TEXT NOT NULL DEFAULT '');
CREATE INDEX reading_state_opened ON reading_state(openedMs DESC);
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
  createdHere INTEGER NOT NULL DEFAULT 0, json TEXT NOT NULL DEFAULT '{}', dateModified TEXT NOT NULL DEFAULT '');
CREATE INDEX annotations_modified ON annotations(dateModified DESC);
CREATE INDEX annotations_parent ON annotations(parentKey, sortIndex);
CREATE INDEX annotations_dirty ON annotations(dirty) WHERE dirty = 1;
CREATE TABLE tracked_attachments (key TEXT PRIMARY KEY);
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
    "createdHere, json, dateModified";

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
        a.dateModified = st.text(15);
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
    const int64_t version = db_.userVersion();
    if (version >= kSchemaVersion) return;
    db_.exec("BEGIN");
    try {
        if (version > 0) {
            // Older schemas are rebuilt: the database is a cache of Zotero and nothing local is
            // lost (no version before 2 ever uploaded or kept unsent work).
            log(LogLevel::Info, "library: schema %lld -> %lld, rebuilding the local copy", static_cast<long long>(version),
                static_cast<long long>(kSchemaVersion));
            std::vector<std::string> tables;
            {
                Stmt st = db_.prepare(
                    "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
                    "AND name NOT LIKE 'item_fts_%'");
                while (st.step()) tables.push_back(st.text(0));
            }
            for (const auto& t : tables) db_.exec("DROP TABLE IF EXISTS \"" + t + "\"");
        }
        db_.exec(kSchema);
        db_.setUserVersion(kSchemaVersion);
        db_.exec("COMMIT");
    } catch (...) {
        try {
            db_.exec("ROLLBACK");
        } catch (const DbError&) {
        }
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
        "   FROM annotations n JOIN attachments a ON n.parentKey = a.key "
        "   WHERE a.parentKey = i.key OR (a.key = i.key AND a.parentKey = '')) "
        "FROM items i WHERE i.key = ?");
    ins.bind(1, itemKey).step();
    Stmt readable = db_.prepare(
        "UPDATE items SET (readableKey, readableType) = (SELECT coalesce(a.key, ''), coalesce(CASE a.contentType "
        "  WHEN 'application/epub+zip' THEN 'EPUB' WHEN 'application/pdf' THEN 'PDF' END, '') "
        "  FROM (SELECT 1) LEFT JOIN (SELECT key, contentType FROM attachments "
        "    WHERE (parentKey = ?1 OR (key = ?1 AND parentKey = '')) "
        "      AND contentType IN ('application/epub+zip', 'application/pdf') "
        "      AND linkMode NOT IN ('linked_file', 'linked_url') "
        "    ORDER BY contentType = 'application/epub+zip' DESC, key LIMIT 1) a) "
        "WHERE key = ?1");
    readable.bind(1, itemKey).step();
}

std::string Library::itemOfAttachment(const std::string& attachmentKey) {
    // A standalone file is its own list entry.
    Stmt st = db_.prepare("SELECT CASE WHEN parentKey = '' THEN key ELSE parentKey END FROM attachments WHERE key = ?");
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
            "tags, dateAdded, dateModified, json, creatorsText, tagsText, year, sortCreator) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(key) DO UPDATE SET version=excluded.version, itemType=excluded.itemType, "
            "title=excluded.title, date=excluded.date, publication=excluded.publication, parentKey=excluded.parentKey, "
            "creators=excluded.creators, collections=excluded.collections, tags=excluded.tags, "
            "dateAdded=excluded.dateAdded, dateModified=excluded.dateModified, json=excluded.json, "
            "creatorsText=excluded.creatorsText, tagsText=excluded.tagsText, year=excluded.year, "
            "sortCreator=excluded.sortCreator");
        st.bind(1, r.key).bind(2, r.version).bind(3, r.itemType).bind(4, r.title).bind(5, r.date);
        st.bind(6, r.publication).bind(7, r.parentKey).bind(8, creators.dump());
        st.bind(9, json(r.collections).dump()).bind(10, json(r.tags).dump()).bind(11, r.dateAdded);
        st.bind(12, r.dateModified).bind(13, r.json).bind(14, creatorsText).bind(15, joined(r.tags));
        std::string sortCreator = r.sortCreator;
        if (sortCreator.empty() && !r.creators.empty())
            sortCreator = r.creators[0].last.empty() ? r.creators[0].name : r.creators[0].last;
        st.bind(16, r.year).bind(17, sortCreator);
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
        const std::string entry = r.parentKey.empty() ? r.key : r.parentKey;
        if (!oldParent.empty() && oldParent != entry) touch(oldParent);
        touch(entry);
    });
}

void Library::upsertAnnotation(const AnnotationRecord& r) {
    guarded([&] {
        Stmt old = db_.prepare("SELECT parentKey FROM annotations WHERE key = ?");
        old.bind(1, r.key);
        const std::string oldAttachment = old.step() ? old.text(0) : std::string{};
        Stmt st = db_.prepare(
            std::string("INSERT INTO annotations (") + kAnnotationColumns +
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(key) DO UPDATE SET parentKey=excluded.parentKey, "
            "type=excluded.type, text=excluded.text, comment=excluded.comment, color=excluded.color, "
            "pageLabel=excluded.pageLabel, sortIndex=excluded.sortIndex, position=excluded.position, "
            "tags=excluded.tags, version=excluded.version, dirty=excluded.dirty, conflict=excluded.conflict, "
            "createdHere=excluded.createdHere, json=excluded.json, dateModified=excluded.dateModified");
        st.bind(1, r.key).bind(2, r.parentKey).bind(3, r.type).bind(4, r.text).bind(5, r.comment);
        st.bind(6, r.color).bind(7, r.pageLabel).bind(8, r.sortIndex).bind(9, r.position);
        st.bind(10, json(r.tags).dump()).bind(11, r.version).bind(12, int64_t{r.dirty});
        st.bind(13, int64_t{r.conflict}).bind(14, int64_t{r.createdHere}).bind(15, r.json.empty() ? "{}" : r.json);
        st.bind(16, r.dateModified);
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

std::string Library::meta(const std::string& key) {
    Stmt st = db_.prepare("SELECT value FROM meta WHERE key = ?");
    st.bind(1, key);
    return st.step() ? st.text(0) : std::string{};
}

void Library::setMeta(const std::string& key, const std::string& value) {
    Stmt st = db_.prepare("INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    st.bind(1, key).bind(2, value).step();
}

int64_t Library::libraryVersion() {
    return guarded([&] {
        const std::string v = meta("libraryVersion");
        return v.empty() ? int64_t{0} : std::stoll(v);
    });
}

void Library::setLibraryVersion(int64_t v) {
    guarded([&] { setMeta("libraryVersion", std::to_string(v)); });
}

bool Library::allAnnotationsOffline() {
    return guarded([&] { return meta("allAnnotationsOffline") == "1"; });
}

void Library::setAllAnnotationsOffline(bool on) {
    guarded([&] {
        setMeta("allAnnotationsOffline", on ? "1" : "0");
        if (!on) setMeta("annotationsVersion", "0");
    });
}

int64_t Library::annotationsVersion() {
    return guarded([&] {
        const std::string v = meta("annotationsVersion");
        return v.empty() ? int64_t{0} : std::stoll(v);
    });
}

void Library::setAnnotationsVersion(int64_t v) {
    guarded([&] { setMeta("annotationsVersion", std::to_string(v)); });
}

void Library::trackAttachment(const std::string& key) {
    guarded([&] {
        Stmt st = db_.prepare("INSERT OR IGNORE INTO tracked_attachments (key) VALUES (?)");
        st.bind(1, key).step();
    });
}

bool Library::isTracked(const std::string& key) {
    return guarded([&] {
        Stmt st = db_.prepare("SELECT 1 FROM tracked_attachments WHERE key = ?");
        st.bind(1, key);
        return st.step();
    });
}

std::vector<std::string> Library::trackedAttachments() {
    return guarded([&] {
        Stmt st = db_.prepare("SELECT key FROM tracked_attachments ORDER BY key");
        std::vector<std::string> out;
        while (st.step()) out.push_back(st.text(0));
        return out;
    });
}

void Library::dropUntrackedAnnotations() {
    guarded([&] {
        Stmt keys = db_.prepare(
            "SELECT key FROM annotations WHERE dirty = 0 AND conflict = 0 "
            "AND parentKey NOT IN (SELECT key FROM tracked_attachments)");
        std::vector<std::string> drop;
        while (keys.step()) drop.push_back(keys.text(0));
        for (const auto& k : drop) removeObject("annotation", k);
    });
}

LibraryStats Library::stats() {
    return guarded([&] {
        Stmt st = db_.prepare(
            "SELECT (SELECT count(*) FROM items), (SELECT count(*) FROM attachments), (SELECT count(*) FROM annotations), "
            "(SELECT count(*) FROM collections), (SELECT count(*) FROM annotations WHERE conflict = 1), "
            "(SELECT count(*) FROM annotations WHERE dirty = 1 AND conflict = 0)");
        st.step();
        return LibraryStats{st.integer(0), st.integer(1), st.integer(2), st.integer(3), st.integer(4), st.integer(5)};
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
                              " FROM attachments WHERE (parentKey = ? OR (key = ? AND parentKey = '')) AND contentType IN ('application/epub+zip', "
                              "'application/pdf') AND linkMode NOT IN ('linked_file', 'linked_url') ORDER BY key");
        st.bind(1, itemKey).bind(2, itemKey);
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

// ---- Screens -------------------------------------------------------------------------------

namespace zk {

namespace {

const char* kRowColumns =
    "i.key, i.title, i.creators, i.year, i.readableKey, i.readableType, coalesce(a.state, 0), "
    "coalesce(rs.progress, -1)";
const char* kRowJoins =
    " LEFT JOIN attachments a ON a.key = i.readableKey LEFT JOIN reading_state rs ON rs.itemKey = i.key ";

std::vector<ItemRow> readRows(Stmt& st) {
    std::vector<ItemRow> out;
    while (st.step()) {
        ItemRecord rec;
        rec.year = st.text(3);
        auto creators = json::parse(st.text(2), nullptr, false);
        if (creators.is_array())
            for (const auto& c : creators)
                rec.creators.push_back({c.value("first", ""), c.value("last", ""), c.value("name", ""), c.value("type", "")});
        ItemRow r;
        r.key = st.text(0);
        r.title = st.text(1);
        r.byline = byline(rec);
        r.attachmentKey = st.text(4);
        r.badge = st.text(5);
        r.fileState = static_cast<AttachmentRecord::State>(st.integer(6));
        r.progress = st.real(7);
        out.push_back(std::move(r));
    }
    return out;
}

// WHERE clause and bindings shared by rows() and countRows().
struct Where {
    std::string sql;
    std::vector<std::string> args;
};

Where whereOf(const ListQuery& q) {
    Where w{" WHERE i.parentKey = ''", {}};
    switch (q.filter) {
        case Filter::Epub: w.sql += " AND i.readableType = 'EPUB'"; break;
        case Filter::Pdf: w.sql += " AND i.readableType = 'PDF'"; break;
        case Filter::Unread: w.sql += " AND NOT EXISTS (SELECT 1 FROM reading_state r WHERE r.itemKey = i.key)"; break;
        case Filter::All: break;
    }
    if (!q.tag.empty()) {
        w.sql += " AND EXISTS (SELECT 1 FROM json_each(i.tags) WHERE value = ?)";
        w.args.push_back(q.tag);
    }
    if (!q.collectionKey.empty()) {
        w.sql += " AND i.key IN (SELECT itemKey FROM item_collections WHERE collectionKey = ?)";
        w.args.push_back(q.collectionKey);
    }
    return w;
}

const char* orderOf(SortBy s) {
    switch (s) {
        case SortBy::Title: return " ORDER BY i.title COLLATE NOCASE, i.key";
        case SortBy::Author: return " ORDER BY i.sortCreator COLLATE NOCASE, i.title COLLATE NOCASE, i.key";
        case SortBy::Year: return " ORDER BY i.year DESC, i.title COLLATE NOCASE, i.key";
        case SortBy::Recent: break;
    }
    return " ORDER BY i.dateAdded DESC, i.key";
}

}  // namespace

std::string byline(const ItemRecord& r) {
    auto nameOf = [](const Creator& c) { return c.last.empty() ? c.name : c.last; };
    std::string who;
    if (r.creators.size() == 1) who = nameOf(r.creators[0]);
    else if (r.creators.size() == 2) who = nameOf(r.creators[0]) + ", " + nameOf(r.creators[1]);
    else if (r.creators.size() > 2) who = nameOf(r.creators[0]) + " et al.";
    if (who.empty()) return r.year;
    return r.year.empty() ? who : who + " · " + r.year;
}

std::vector<ItemRow> Library::rows(const ListQuery& q) {
    return guarded([&] {
        Where w = whereOf(q);
        Stmt st = db_.prepare(std::string("SELECT ") + kRowColumns + " FROM items i" + kRowJoins + w.sql + orderOf(q.sort) +
                              " LIMIT ? OFFSET ?");
        int n = 1;
        for (const auto& a : w.args) st.bind(n++, a);
        st.bind(n++, int64_t{q.limit}).bind(n, int64_t{q.offset});
        return readRows(st);
    });
}

int64_t Library::countRows(const ListQuery& q) {
    return guarded([&] {
        Where w = whereOf(q);
        Stmt st = db_.prepare("SELECT count(*) FROM items i" + w.sql);
        int n = 1;
        for (const auto& a : w.args) st.bind(n++, a);
        st.step();
        return st.integer(0);
    });
}

std::vector<ItemRow> Library::searchRows(const std::string& query, int offset, int limit) {
    const std::string fts = ftsQuery(query);
    if (fts.empty()) return {};
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kRowColumns + " FROM item_fts JOIN items i ON i.id = item_fts.rowid" +
                              kRowJoins + " WHERE item_fts MATCH ? AND i.parentKey = '' ORDER BY rank LIMIT ? OFFSET ?");
        st.bind(1, fts).bind(2, int64_t{limit}).bind(3, int64_t{offset});
        return readRows(st);
    });
}

std::vector<std::pair<std::string, int64_t>> Library::topTags(int n) {
    return guarded([&] {
        Stmt st = db_.prepare(
            "SELECT t.value, count(*) FROM items i, json_each(i.tags) t WHERE i.parentKey = '' "
            "GROUP BY t.value ORDER BY 2 DESC, 1 LIMIT ?");
        st.bind(1, int64_t{n});
        std::vector<std::pair<std::string, int64_t>> out;
        while (st.step()) out.emplace_back(st.text(0), st.integer(1));
        return out;
    });
}

std::vector<CollectionEntry> Library::childCollections(const std::string& parentKey) {
    return guarded([&] {
        Stmt st = db_.prepare(
            "SELECT c.key, c.name, c.parentKey, c.version, "
            "(SELECT count(*) FROM item_collections ic WHERE ic.collectionKey = c.key), "
            "EXISTS (SELECT 1 FROM collections d WHERE d.parentKey = c.key) "
            "FROM collections c WHERE c.parentKey = ? ORDER BY c.name COLLATE NOCASE");
        st.bind(1, parentKey);
        std::vector<CollectionEntry> out;
        while (st.step())
            out.push_back({{st.text(0), st.text(1), st.text(2), st.integer(3)}, st.integer(4), st.integer(5) != 0});
        return out;
    });
}

std::vector<NoteRow> Library::recentAnnotations(int offset, int limit) {
    return guarded([&] {
        std::string cols;
        for (const char* c : {"key", "parentKey", "type", "text", "comment", "color", "pageLabel", "sortIndex", "position",
                              "tags", "version", "dirty", "conflict", "createdHere", "json", "dateModified"})
            cols += std::string(cols.empty() ? "" : ", ") + "n." + c;
        Stmt st = db_.prepare("SELECT " + cols +
                              ", coalesce(i.key, ''), coalesce(i.title, '') FROM annotations n "
                              "LEFT JOIN attachments a ON a.key = n.parentKey "
                              "LEFT JOIN items i ON i.key = CASE WHEN a.parentKey = '' THEN a.key ELSE a.parentKey END "
                              "ORDER BY n.dateModified DESC, n.key LIMIT ? OFFSET ?");
        st.bind(1, int64_t{limit}).bind(2, int64_t{offset});
        std::vector<NoteRow> out;
        // readAnnotations consumes the statement; read the two extra columns alongside.
        while (st.step()) {
            NoteRow row;
            AnnotationRecord& a = row.annotation;
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
            a.dateModified = st.text(15);
            row.itemKey = st.text(16);
            row.itemTitle = st.text(17);
            out.push_back(std::move(row));
        }
        return out;
    });
}

int64_t Library::countAnnotations() {
    return guarded([&] {
        Stmt st = db_.prepare("SELECT count(*) FROM annotations");
        st.step();
        return st.integer(0);
    });
}

void Library::markOpened(const std::string& itemKey, const std::string& attachmentKey, int64_t nowMs) {
    guarded([&] {
        Stmt st = db_.prepare(
            "INSERT INTO reading_state (itemKey, attachmentKey, openedMs) VALUES (?, ?, ?) ON CONFLICT(itemKey) DO UPDATE "
            "SET attachmentKey = excluded.attachmentKey, openedMs = excluded.openedMs");
        st.bind(1, itemKey).bind(2, attachmentKey).bind(3, nowMs).step();
    });
}

std::optional<ReadingState> Library::lastOpened() {
    return guarded([&]() -> std::optional<ReadingState> {
        Stmt st = db_.prepare(
            "SELECT itemKey, attachmentKey, openedMs, progress, location FROM reading_state ORDER BY openedMs DESC LIMIT 1");
        if (!st.step()) return std::nullopt;
        return ReadingState{st.text(0), st.text(1), st.integer(2), st.real(3), st.text(4)};
    });
}

void Library::setFileState(const std::string& attachmentKey, AttachmentRecord::State state, const std::string& localPath) {
    guarded([&] {
        Stmt st = db_.prepare("UPDATE attachments SET state = ?, localPath = ? WHERE key = ?");
        st.bind(1, static_cast<int64_t>(state)).bind(2, localPath).bind(3, attachmentKey).step();
    });
}

std::vector<AttachmentRecord> Library::downloadedAttachments() {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAttachmentColumns + " FROM attachments WHERE state = ? ORDER BY key");
        st.bind(1, static_cast<int64_t>(AttachmentRecord::State::Local));
        return readAttachments(st);
    });
}

}  // namespace zk
