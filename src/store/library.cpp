#include "store/library.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <glib.h>

#include <chrono>
#include <ctime>
#include <random>

namespace zk {

using nlohmann::json;

namespace {

constexpr int64_t kSchemaVersion = 9;

// Deletes an attachment's downloaded copy: the file, and its <files>/<attachmentKey>/ folder.
void deleteDownloadedCopy(const std::string& attachmentKey, const std::string& localPath) {
    if (attachmentKey.empty() || localPath.empty()) return;
    const std::filesystem::path file(localPath);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    if (file.parent_path().filename() == attachmentKey) std::filesystem::remove_all(file.parent_path(), ec);
}

// The current schema (PRAGMA user_version = kSchemaVersion). Text-heavy columns default to '' so reads never see NULL.
// items.id is the stable rowid the FTS index points at (a VACUUM can't renumber it).
// readableKey/readableType: the item's file to open (EPUB preferred), kept by reindex().
// titleKey/authorKey: listSortKey() of the title and of sortCreator, kept by upsertItem().
const char* kSchema = R"sql(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE items (
  id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE, version INTEGER NOT NULL DEFAULT 0, itemType TEXT NOT NULL DEFAULT '',
  title TEXT NOT NULL DEFAULT '', date TEXT NOT NULL DEFAULT '', publication TEXT NOT NULL DEFAULT '',
  parentKey TEXT NOT NULL DEFAULT '', creators TEXT NOT NULL DEFAULT '[]', collections TEXT NOT NULL DEFAULT '[]',
  tags TEXT NOT NULL DEFAULT '[]', dateAdded TEXT NOT NULL DEFAULT '', dateModified TEXT NOT NULL DEFAULT '',
  json TEXT NOT NULL DEFAULT '{}', creatorsText TEXT NOT NULL DEFAULT '', tagsText TEXT NOT NULL DEFAULT '',
  year TEXT NOT NULL DEFAULT '', sortCreator TEXT NOT NULL DEFAULT '', readableKey TEXT NOT NULL DEFAULT '',
  readableType TEXT NOT NULL DEFAULT '', titleKey TEXT NOT NULL DEFAULT '', authorKey TEXT NOT NULL DEFAULT '');
CREATE INDEX items_recent ON items(parentKey, dateAdded DESC);
CREATE INDEX items_title ON items(parentKey, title COLLATE NOCASE);
CREATE INDEX items_author ON items(parentKey, sortCreator COLLATE NOCASE, title COLLATE NOCASE);
CREATE INDEX items_year ON items(parentKey, year DESC, titleKey);
CREATE INDEX items_readable ON items(parentKey, readableType);
CREATE INDEX items_titlekey ON items(parentKey, titleKey);
CREATE INDEX items_authorkey ON items(parentKey, authorKey, titleKey);
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
  createdHere INTEGER NOT NULL DEFAULT 0, json TEXT NOT NULL DEFAULT '{}', dateModified TEXT NOT NULL DEFAULT '',
  deletedHere INTEGER NOT NULL DEFAULT 0, localOnly INTEGER NOT NULL DEFAULT 0, conflictOf TEXT NOT NULL DEFAULT '');
CREATE INDEX annotations_modified ON annotations(dateModified DESC);
CREATE INDEX annotations_parent ON annotations(parentKey, sortIndex);
CREATE INDEX annotations_dirty ON annotations(dirty) WHERE dirty = 1;
CREATE TABLE notes (
  key TEXT PRIMARY KEY, parentKey TEXT NOT NULL DEFAULT '', html TEXT NOT NULL DEFAULT '', version INTEGER NOT NULL DEFAULT 0,
  json TEXT NOT NULL DEFAULT '{}', dateModified TEXT NOT NULL DEFAULT '', createdBy TEXT NOT NULL DEFAULT '');
CREATE INDEX notes_parent ON notes(parentKey);
CREATE TABLE tracked_attachments (key TEXT PRIMARY KEY);
CREATE TABLE object_versions (key TEXT PRIMARY KEY, version INTEGER NOT NULL);
CREATE TABLE reading_positions (attachmentKey TEXT PRIMARY KEY, remote TEXT NOT NULL DEFAULT '',
  remoteVersion INTEGER NOT NULL DEFAULT 0, local TEXT NOT NULL DEFAULT '', dirty INTEGER NOT NULL DEFAULT 0,
  remoteNew INTEGER NOT NULL DEFAULT 0, lastSent TEXT NOT NULL DEFAULT '');
CREATE TABLE read_marks (itemKey TEXT PRIMARY KEY, read INTEGER NOT NULL, tag TEXT NOT NULL);
CREATE VIRTUAL TABLE item_fts USING fts5(title, creators, publication, tags,
  tokenize='unicode61 remove_diacritics 2');
)sql";

// Highlight and note search (the Notes tab): kept by triggers, so every write to annotations counts.
const char* kNoteSearch = R"sql(
CREATE VIRTUAL TABLE IF NOT EXISTS note_fts USING fts5(text, comment, tags, tokenize='unicode61 remove_diacritics 2');
CREATE TRIGGER IF NOT EXISTS annotations_fts_insert AFTER INSERT ON annotations BEGIN
  INSERT INTO note_fts(rowid, text, comment, tags) VALUES (new.rowid, new.text, new.comment, new.tags);
END;
CREATE TRIGGER IF NOT EXISTS annotations_fts_delete AFTER DELETE ON annotations BEGIN
  DELETE FROM note_fts WHERE rowid = old.rowid;
END;
CREATE TRIGGER IF NOT EXISTS annotations_fts_update AFTER UPDATE ON annotations BEGIN
  DELETE FROM note_fts WHERE rowid = old.rowid;
  INSERT INTO note_fts(rowid, text, comment, tags) VALUES (new.rowid, new.text, new.comment, new.tags);
END;
)sql";

// Changes plugins asked for (Library::queuePluginChange). kind and state as plugin_changes.h names
// them; seq is the order they were asked in.
const char* kPluginChanges = R"sql(
CREATE TABLE IF NOT EXISTS plugin_changes (id TEXT PRIMARY KEY, seq INTEGER NOT NULL, pluginId TEXT NOT NULL,
  kind TEXT NOT NULL, itemKey TEXT NOT NULL DEFAULT '', parentKey TEXT NOT NULL DEFAULT '', value TEXT NOT NULL DEFAULT '',
  dependsOn TEXT NOT NULL DEFAULT '', state TEXT NOT NULL DEFAULT 'pending', message TEXT NOT NULL DEFAULT '',
  createdMs INTEGER NOT NULL DEFAULT 0, sentMs INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS plugin_changes_item ON plugin_changes(itemKey, state);
CREATE INDEX IF NOT EXISTS plugin_changes_plugin ON plugin_changes(pluginId, seq);
CREATE INDEX IF NOT EXISTS plugin_changes_state ON plugin_changes(state, seq);
CREATE INDEX IF NOT EXISTS plugin_changes_depends ON plugin_changes(dependsOn) WHERE dependsOn != '';
)sql";

// Not in the trash by a plugin change still to be sent (the item is `alias`.key): such an item
// is hidden from lists, search and details until the change is sent or withdrawn.
std::string notTrashed(const std::string& keyColumn) {
    return " AND NOT EXISTS (SELECT 1 FROM plugin_changes p WHERE p.itemKey = " + keyColumn +
           " AND p.kind = 'trash' AND p.state = 'pending')";
}

// Version 6 on an older database: book search without highlight text (then kNoteSearch, filled).
const char* kVersion6 = R"sql(
DROP TABLE IF EXISTS item_fts;
CREATE VIRTUAL TABLE item_fts USING fts5(title, creators, publication, tags,
  tokenize='unicode61 remove_diacritics 2');
)sql";

const char* kItemColumns =
    "items.key, items.version, items.itemType, items.title, items.date, items.publication, items.parentKey, "
    "items.creators, items.collections, items.tags, items.dateAdded, items.dateModified, items.json, items.year";

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
    r.year = st.text(13);
    return r;
}

std::vector<ItemRecord> readItems(Stmt& st) {
    std::vector<ItemRecord> out;
    while (st.step()) out.push_back(readItem(st));
    return out;
}

const char* kAnnotationColumns =
    "key, parentKey, type, text, comment, color, pageLabel, sortIndex, position, tags, version, dirty, conflict, "
    "createdHere, json, dateModified, deletedHere, localOnly, conflictOf";

// Reads the kAnnotationColumns of the current row, starting at column `at`.
AnnotationRecord readAnnotation(Stmt& st, int at = 0) {
    AnnotationRecord a;
    a.key = st.text(at + 0);
    a.parentKey = st.text(at + 1);
    a.type = st.text(at + 2);
    a.text = st.text(at + 3);
    a.comment = st.text(at + 4);
    a.color = st.text(at + 5);
    a.pageLabel = st.text(at + 6);
    a.sortIndex = st.text(at + 7);
    a.position = st.text(at + 8);
    a.tags = strings(st.text(at + 9));
    a.version = st.integer(at + 10);
    a.dirty = st.integer(at + 11) != 0;
    a.conflict = st.integer(at + 12) != 0;
    a.createdHere = st.integer(at + 13) != 0;
    a.json = st.text(at + 14);
    a.dateModified = st.text(at + 15);
    a.deletedHere = st.integer(at + 16) != 0;
    a.localOnly = st.integer(at + 17) != 0;
    a.conflictOf = st.text(at + 18);
    return a;
}

std::vector<AnnotationRecord> readAnnotations(Stmt& st) {
    std::vector<AnnotationRecord> out;
    while (st.step()) out.push_back(readAnnotation(st));
    return out;
}

// "2026-11-07T09:30:00Z", as Zotero writes dateModified.
std::string nowUtc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
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

const char* kChangeColumns = "id, pluginId, kind, itemKey, value, parentKey, dependsOn, state, message, createdMs, sentMs";

PluginChange readChange(const Stmt& st) {
    PluginChange c;
    c.id = st.text(0);
    c.pluginId = st.text(1);
    kindFromName(st.text(2), c.kind);
    c.itemKey = st.text(3);
    c.value = st.text(4);
    c.parentKey = st.text(5);
    c.dependsOn = st.text(6);
    stateFromName(st.text(7), c.state);
    c.message = st.text(8);
    c.createdMs = st.integer(9);
    c.sentMs = st.integer(10);
    return c;
}

// Zotero's data JSON with its "tags" and "collections" filled from these if it has none (a record
// made without Zotero's data): then the JSON always holds the base the pending changes apply to.
std::string withBaseLists(const std::string& data, const std::vector<std::string>& tags,
                          const std::vector<std::string>& collections) {
    json d = json::parse(data, nullptr, false);
    if (!d.is_object()) d = json::object();
    if (d.contains("tags") && d["tags"].is_array() && d.contains("collections") && d["collections"].is_array())
        return data;
    if (!d.contains("tags") || !d["tags"].is_array()) {
        d["tags"] = json::array();
        for (const auto& t : tags) d["tags"].push_back({{"tag", t}});
    }
    if (!d.contains("collections") || !d["collections"].is_array()) d["collections"] = collections;
    return d.dump();
}

// The tags and collections in Zotero's data JSON; false if it has none.
bool baseLists(const std::string& data, std::vector<std::string>& tags, std::vector<std::string>& collections) {
    json d = json::parse(data, nullptr, false);
    if (!d.is_object() || !d.contains("tags") || !d["tags"].is_array() || !d.contains("collections") ||
        !d["collections"].is_array())
        return false;
    tags.clear();
    for (const auto& t : d["tags"])
        if (t.is_object() && t.contains("tag") && t["tag"].is_string()) tags.push_back(t["tag"].get<std::string>());
    collections.clear();
    for (const auto& c : d["collections"])
        if (c.is_string()) collections.push_back(c.get<std::string>());
    return true;
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

namespace {

bool isMark(gunichar c) {
    const GUnicodeType t = g_unichar_type(c);
    return t == G_UNICODE_NON_SPACING_MARK || t == G_UNICODE_SPACING_MARK || t == G_UNICODE_ENCLOSING_MARK;
}

// Case and accents folded away: "Über" -> "uber", "Ёлка" -> "елка".
std::string folded(const char* text) {
    gchar* nfd = g_utf8_normalize(text, -1, G_NORMALIZE_NFD);
    if (!nfd) return "";
    std::string bare;
    for (const gchar* p = nfd; *p; p = g_utf8_next_char(p)) {
        const gunichar c = g_utf8_get_char(p);
        if (isMark(c)) continue;
        char buf[6];
        bare.append(buf, static_cast<size_t>(g_unichar_to_utf8(c, buf)));
    }
    g_free(nfd);
    gchar* f = g_utf8_casefold(bare.c_str(), -1);
    std::string out = f ? f : "";
    g_free(f);
    return out;
}

// "1A", "2Ж" or "3#": the letter a title or name is filed under.
std::string initialOf(gunichar c) {
    if (g_unichar_isdigit(c)) return "3#";
    gunichar u = g_unichar_toupper(c);
    if (u == 0x0401) u = 0x0415;  // Ё with Е
    char buf[6];
    if (u >= 0x0410 && u <= 0x042F) return "2" + std::string(buf, static_cast<size_t>(g_unichar_to_utf8(u, buf)));
    switch (u) {  // letters without a decomposition
        case 0x00C6: return "1A";                          // Æ
        case 0x0152: case 0x00D8: return "1O";             // Œ Ø
        case 0x0141: return "1L";                          // Ł
        case 0x0110: case 0x00D0: return "1D";             // Đ Ð
        case 0x1E9E: case 0x00DF: return "1S";             // ẞ ß
        default: break;
    }
    // The base letter of an accented one (É -> E). g_utf8_normalize, not g_unichar_fully_decompose:
    // the Kindle's GLib predates 2.30.
    if (char* nfd = g_utf8_normalize(buf, g_unichar_to_utf8(u, buf), G_NORMALIZE_NFD)) {
        u = g_unichar_toupper(g_utf8_get_char(nfd));
        g_free(nfd);
    }
    if (u >= 'A' && u <= 'Z') return std::string("1") + static_cast<char>(u);
    return "3#";
}

}  // namespace

std::string listSortKey(const std::string& text) {
    if (!g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr)) return "3#\x1f";
    const char* p = text.c_str();
    while (*p && !g_unichar_isalnum(g_utf8_get_char(p))) p = g_utf8_next_char(p);  // quotes, brackets, spaces
    if (!*p) return "3#\x1f" + folded(text.c_str());
    return initialOf(g_utf8_get_char(p)) + "\x1f" + folded(p);
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
        if (version >= 2 && version <= 8) {
            // Versions 2 to 8 may hold unsent Kindle work, so they are extended in place, never rebuilt.
            if (version == 2)
                db_.exec(
                    "ALTER TABLE annotations ADD COLUMN deletedHere INTEGER NOT NULL DEFAULT 0;"
                    "ALTER TABLE annotations ADD COLUMN localOnly INTEGER NOT NULL DEFAULT 0;"
                    "ALTER TABLE annotations ADD COLUMN conflictOf TEXT NOT NULL DEFAULT '';");
            db_.exec(  // version 4: reading positions
                "CREATE TABLE IF NOT EXISTS reading_positions (attachmentKey TEXT PRIMARY KEY, remote TEXT NOT NULL "
                "DEFAULT '', remoteVersion INTEGER NOT NULL DEFAULT 0, local TEXT NOT NULL DEFAULT '', dirty INTEGER "
                "NOT NULL DEFAULT 0, remoteNew INTEGER NOT NULL DEFAULT 0, lastSent TEXT NOT NULL DEFAULT '');");
            db_.exec(  // version 5: read marks
                "CREATE TABLE IF NOT EXISTS read_marks (itemKey TEXT PRIMARY KEY, read INTEGER NOT NULL, tag TEXT NOT NULL);");
            if (version < 6) db_.exec(kVersion6);  // a version 6 database has this index already
            db_.exec(  // version 7: Zotero notes
                "CREATE TABLE IF NOT EXISTS notes (key TEXT PRIMARY KEY, parentKey TEXT NOT NULL DEFAULT '', html TEXT NOT "
                "NULL DEFAULT '', version INTEGER NOT NULL DEFAULT 0, json TEXT NOT NULL DEFAULT '{}', dateModified TEXT NOT "
                "NULL DEFAULT '', createdBy TEXT NOT NULL DEFAULT '');"
                "CREATE INDEX IF NOT EXISTS notes_parent ON notes(parentKey);");
            // Earlier pulls recorded their notes' versions without keeping them: the next pull lists them again.
            if (version < 7)
                db_.exec("INSERT INTO meta (key, value) VALUES ('notesBackfill', '1') "
                         "ON CONFLICT(key) DO UPDATE SET value = '1'");
            auto has = [&](const char* table) {
                Stmt st = db_.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?");
                st.bind(1, std::string(table));
                return st.step();
            };
            if (has("items") && version < 6)  // kVersion6 emptied it
                db_.exec("INSERT INTO item_fts(rowid, title, creators, publication, tags) "
                         "SELECT rowid, title, creatorsText, publication, tagsText FROM items;");
            if (!has("note_fts")) {
                db_.exec(kNoteSearch);
                db_.exec("INSERT INTO note_fts(rowid, text, comment, tags) SELECT rowid, text, comment, tags FROM annotations;");
            }
            db_.exec(kPluginChanges);  // version 8: plugin changes
            auto hasColumn = [&](const char* column) {
                Stmt st = db_.prepare("SELECT 1 FROM pragma_table_info('items') WHERE name = ?");
                st.bind(1, std::string(column));
                return st.step();
            };
            if (has("items") && version < 9) {  // version 9: sort keys for Title and Author, with their sections
                for (const char* column : {"titleKey", "authorKey"})
                    if (!hasColumn(column))
                        db_.exec(std::string("ALTER TABLE items ADD COLUMN ") + column + " TEXT NOT NULL DEFAULT '';");
                db_.exec("CREATE INDEX IF NOT EXISTS items_titlekey ON items(parentKey, titleKey);"
                         "CREATE INDEX IF NOT EXISTS items_authorkey ON items(parentKey, authorKey, titleKey);"
                         "DROP INDEX IF EXISTS items_year;"
                         "CREATE INDEX items_year ON items(parentKey, year DESC, titleKey);");
                std::vector<std::array<std::string, 3>> keys;
                {
                    Stmt st = db_.prepare("SELECT key, title, sortCreator FROM items");
                    while (st.step()) keys.push_back({st.text(0), listSortKey(st.text(1)), listSortKey(st.text(2))});
                }
                Stmt set = db_.prepare("UPDATE items SET titleKey = ?, authorKey = ? WHERE key = ?");
                for (const auto& k : keys) {
                    set.bind(1, k[1]).bind(2, k[2]).bind(3, k[0]).step();
                    set.reset();
                }
            }
            db_.setUserVersion(kSchemaVersion);
            db_.exec("COMMIT");
            return;
        }
        if (version > 0) {
            // Older schemas are rebuilt: the database is a cache of Zotero and nothing local is
            // lost (no version before 2 ever uploaded or kept unsent work).
            log(LogLevel::Info, "library: schema %lld -> %lld, rebuilding the local copy", static_cast<long long>(version),
                static_cast<long long>(kSchemaVersion));
            std::vector<std::string> tables;
            {
                Stmt st = db_.prepare(
                    "SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
                    "AND name NOT LIKE 'item_fts_%' AND name NOT LIKE 'note_fts_%'");
                while (st.step()) tables.push_back(st.text(0));
            }
            for (const auto& t : tables) db_.exec("DROP TABLE IF EXISTS \"" + t + "\"");
        }
        db_.exec(kSchema);
        db_.exec(kNoteSearch);
        db_.exec(kPluginChanges);
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
        "INSERT INTO item_fts(rowid, title, creators, publication, tags) "
        "SELECT i.rowid, i.title, i.creatorsText, i.publication, i.tagsText FROM items i WHERE i.key = ?");
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
            "tags, dateAdded, dateModified, json, creatorsText, tagsText, year, sortCreator, titleKey, authorKey) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(key) DO UPDATE SET version=excluded.version, itemType=excluded.itemType, "
            "title=excluded.title, date=excluded.date, publication=excluded.publication, parentKey=excluded.parentKey, "
            "creators=excluded.creators, collections=excluded.collections, tags=excluded.tags, "
            "dateAdded=excluded.dateAdded, dateModified=excluded.dateModified, json=excluded.json, "
            "creatorsText=excluded.creatorsText, tagsText=excluded.tagsText, year=excluded.year, "
            "sortCreator=excluded.sortCreator, titleKey=excluded.titleKey, authorKey=excluded.authorKey");
        st.bind(1, r.key).bind(2, r.version).bind(3, r.itemType).bind(4, r.title).bind(5, r.date);
        st.bind(6, r.publication).bind(7, r.parentKey).bind(8, creators.dump());
        // Pending plugin changes on it are applied again below, to Zotero's lists (kept in its json).
        Stmt pending = db_.prepare("SELECT 1 FROM plugin_changes WHERE itemKey = ? AND state = 'pending'");
        pending.bind(1, r.key);
        const bool overlaid = pending.step();
        st.bind(9, json(r.collections).dump()).bind(10, json(r.tags).dump()).bind(11, r.dateAdded);
        st.bind(12, r.dateModified).bind(13, overlaid ? withBaseLists(r.json, r.tags, r.collections) : r.json);
        st.bind(14, creatorsText).bind(15, joined(r.tags));
        std::string sortCreator = r.sortCreator;
        if (sortCreator.empty() && !r.creators.empty())
            sortCreator = r.creators[0].last.empty() ? r.creators[0].name : r.creators[0].last;
        st.bind(16, r.year).bind(17, sortCreator).bind(18, listSortKey(r.title)).bind(19, listSortKey(sortCreator));
        st.step();
        Stmt clear = db_.prepare("DELETE FROM item_collections WHERE itemKey = ?");
        clear.bind(1, r.key).step();
        for (const auto& c : r.collections) {
            Stmt add = db_.prepare("INSERT OR IGNORE INTO item_collections (itemKey, collectionKey) VALUES (?, ?)");
            add.bind(1, r.key).bind(2, c).step();
        }
        if (overlaid) overlayPluginChanges(r.key);
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
            ") VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(key) DO UPDATE SET parentKey=excluded.parentKey, "
            "type=excluded.type, text=excluded.text, comment=excluded.comment, color=excluded.color, "
            "pageLabel=excluded.pageLabel, sortIndex=excluded.sortIndex, position=excluded.position, "
            "tags=excluded.tags, version=excluded.version, dirty=excluded.dirty, conflict=excluded.conflict, "
            "createdHere=excluded.createdHere, json=excluded.json, dateModified=excluded.dateModified, "
            "deletedHere=excluded.deletedHere, localOnly=excluded.localOnly, conflictOf=excluded.conflictOf");
        st.bind(1, r.key).bind(2, r.parentKey).bind(3, r.type).bind(4, r.text).bind(5, r.comment);
        st.bind(6, r.color).bind(7, r.pageLabel).bind(8, r.sortIndex).bind(9, r.position);
        st.bind(10, json(r.tags).dump()).bind(11, r.version).bind(12, int64_t{r.dirty});
        st.bind(13, int64_t{r.conflict}).bind(14, int64_t{r.createdHere}).bind(15, r.json.empty() ? "{}" : r.json);
        st.bind(16, r.dateModified).bind(17, int64_t{r.deletedHere}).bind(18, int64_t{r.localOnly});
        st.bind(19, r.conflictOf);
        st.step();
        if (!oldAttachment.empty() && oldAttachment != r.parentKey) touch(itemOfAttachment(oldAttachment));
        touch(itemOfAttachment(r.parentKey));
    });
}

void Library::upsertNote(const NoteRecord& r) {
    guarded([&] {
        Stmt st = db_.prepare(
            "INSERT INTO notes (key, parentKey, html, version, json, dateModified, createdBy) VALUES (?,?,?,?,?,?,?) "
            "ON CONFLICT(key) DO UPDATE SET parentKey=excluded.parentKey, html=excluded.html, version=excluded.version, "
            "json=excluded.json, dateModified=excluded.dateModified, "
            "createdBy=CASE WHEN excluded.createdBy = '' THEN notes.createdBy ELSE excluded.createdBy END");
        st.bind(1, r.key).bind(2, r.parentKey).bind(3, r.html).bind(4, r.version);
        st.bind(5, r.json.empty() ? "{}" : r.json).bind(6, r.dateModified).bind(7, r.createdBy);
        st.step();
    });
}

namespace {
const char* kNoteColumns = "key, parentKey, html, version, json, dateModified, createdBy";
// Hidden while a plugin's deletion of it is still to be sent.
const char* kNoteNotDeleted =
    " AND NOT EXISTS (SELECT 1 FROM plugin_changes p WHERE p.itemKey = notes.key AND p.kind = 'deleteNote' "
    "AND p.state = 'pending')";
NoteRecord readNote(Stmt& st) {
    NoteRecord n;
    n.key = st.text(0);
    n.parentKey = st.text(1);
    n.html = st.text(2);
    n.version = st.integer(3);
    n.json = st.text(4);
    n.dateModified = st.text(5);
    n.createdBy = st.text(6);
    return n;
}
}  // namespace

std::optional<NoteRecord> Library::note(const std::string& key) {
    return guarded([&]() -> std::optional<NoteRecord> {
        Stmt st = db_.prepare(std::string("SELECT ") + kNoteColumns + " FROM notes WHERE key = ?" + kNoteNotDeleted);
        st.bind(1, key);
        if (!st.step()) return std::nullopt;
        return readNote(st);
    });
}

std::optional<NoteRecord> Library::zoteroNote(const std::string& key) {
    return guarded([&]() -> std::optional<NoteRecord> {
        Stmt st = db_.prepare(std::string("SELECT ") + kNoteColumns + " FROM notes WHERE key = ?");
        st.bind(1, key);
        if (!st.step()) return std::nullopt;
        return readNote(st);
    });
}

std::vector<NoteRecord> Library::notes(const std::string& itemKey) {
    return guarded([&] {
        std::vector<NoteRecord> out;
        Stmt st = db_.prepare(std::string("SELECT ") + kNoteColumns + " FROM notes WHERE parentKey = ?" + kNoteNotDeleted +
                              " ORDER BY key");
        st.bind(1, itemKey);
        while (st.step()) out.push_back(readNote(st));
        return out;
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
        if (type == "item" || type == "note") {
            // Plugin changes still to be sent to it (or adding a note to it) can't be any more.
            std::vector<PluginChange> stale;
            Stmt st = db_.prepare(std::string("SELECT ") + kChangeColumns +
                                  " FROM plugin_changes WHERE state = 'pending' AND (itemKey = ?1 OR "
                                  "(kind = 'addNote' AND parentKey = ?1)) ORDER BY seq DESC");
            st.bind(1, key);
            while (st.step()) stale.push_back(readChange(st));
            for (const auto& c : stale) {
                auto now = pluginChange(c.id);
                if (!now || now->state != PluginChange::State::Pending) continue;
                // Its own removal (a trash, a note deletion) has happened: that one is done.
                const bool removal = c.itemKey == key && ((type == "item" && c.kind == PluginChange::Kind::Trash) ||
                                                          (type == "note" && c.kind == PluginChange::Kind::DeleteNote));
                bool allowed = true;  // a note deletion: only of a note the same plugin added
                if (removal && c.kind == PluginChange::Kind::DeleteNote) {
                    Stmt by = db_.prepare("SELECT createdBy FROM notes WHERE key = ?");
                    by.bind(1, key);
                    allowed = by.step() && !by.text(0).empty() && by.text(0) == c.pluginId;
                }
                if (removal && !allowed) {
                    withdrawPluginChange(c, PluginChange::State::Refused, "Only a note the plugin added can be deleted.");
                    continue;
                }
                if (removal) {
                    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                    Stmt done = db_.prepare("UPDATE plugin_changes SET state = 'sent', message = '', sentMs = ? WHERE id = ?");
                    done.bind(1, ms).bind(2, c.id).step();
                    continue;
                }
                withdrawPluginChange(c, PluginChange::State::Refused, "It was deleted in Zotero.",
                                     "It was deleted in Zotero.");
            }
        }
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
            // Its files go from the Kindle too (deleted or trashed in Zotero). The attachment rows
            // stay until Zotero deletes them, so an item restored from the trash downloads again.
            Stmt kids = db_.prepare("SELECT key, localPath FROM attachments WHERE parentKey = ? AND localPath != ''");
            kids.bind(1, key);
            std::vector<std::pair<std::string, std::string>> copies;
            while (kids.step()) copies.emplace_back(kids.text(0), kids.text(1));
            for (const auto& [att, localPath] : copies) {
                deleteDownloadedCopy(att, localPath);
                Stmt remote = db_.prepare("UPDATE attachments SET state = ?, localPath = '' WHERE key = ?");
                remote.bind(1, static_cast<int64_t>(AttachmentRecord::State::Remote)).bind(2, att).step();
            }
        } else if (type == "attachment") {
            const std::string parent = itemOfAttachment(key);
            Stmt file = db_.prepare("SELECT localPath FROM attachments WHERE key = ?");
            file.bind(1, key);
            if (file.step()) deleteDownloadedCopy(key, file.text(0));  // else nothing could find it again
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
        } else if (type == "note") {
            Stmt st = db_.prepare("DELETE FROM notes WHERE key = ?");
            st.bind(1, key).step();
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

std::string Library::metaValue(const std::string& key) {
    return guarded([&] { return meta(key); });
}

void Library::setMetaValue(const std::string& key, const std::string& value) {
    guarded([&] { setMeta(key, value); });
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
            "SELECT (SELECT count(*) FROM items), (SELECT count(*) FROM attachments), (SELECT count(*) FROM annotations WHERE deletedHere = 0), "
            "(SELECT count(*) FROM collections), (SELECT count(*) FROM annotations WHERE conflict = 1), "
            "(SELECT count(*) FROM annotations WHERE dirty = 1 AND conflict = 0)");
        st.step();
        return LibraryStats{st.integer(0), st.integer(1), st.integer(2), st.integer(3), st.integer(4), st.integer(5)};
    });
}

void Library::forEachItemData(const std::function<void(const std::string&, const std::string&)>& fn) {
    guarded([&] {
        Stmt st = db_.prepare("SELECT key, json FROM items WHERE parentKey = ''" + notTrashed("items.key"));
        while (st.step()) fn(st.text(0), st.text(1));
    });
}

int64_t Library::dataVersion() {
    return guarded([&] {
        Stmt st = db_.prepare("PRAGMA data_version");
        return st.step() ? st.integer(0) : int64_t{0};
    });
}

std::vector<ItemRecord> Library::recentItems(int limit) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kItemColumns +
                              " FROM items WHERE parentKey = ''" + notTrashed("items.key") +
                              " ORDER BY dateAdded DESC LIMIT ?");
        st.bind(1, int64_t{limit});
        return readItems(st);
    });
}

std::vector<ItemRecord> Library::itemsInCollection(const std::string& key) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kItemColumns +
                              " FROM item_collections c JOIN items ON items.key = c.itemKey WHERE c.collectionKey = ?" +
                              notTrashed("items.key") + " ORDER BY items.title COLLATE NOCASE");
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
                              " FROM item_fts JOIN items ON items.rowid = item_fts.rowid WHERE item_fts MATCH ?" +
                              notTrashed("items.key") + " ORDER BY rank LIMIT ?");
        st.bind(1, fts).bind(2, int64_t{limit});
        return readItems(st);
    });
}

std::vector<AnnotationRecord> Library::annotationsFor(const std::string& attachmentKey) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAnnotationColumns +
                              " FROM annotations WHERE parentKey = ? AND deletedHere = 0 ORDER BY sortIndex, key");
        st.bind(1, attachmentKey);
        return readAnnotations(st);
    });
}

std::vector<AnnotationRecord> Library::dirtyAnnotations() {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAnnotationColumns +
                              " FROM annotations WHERE dirty = 1 AND deletedHere = 0 ORDER BY parentKey, sortIndex");
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

std::vector<AttachmentRecord> Library::attachmentsOf(const std::string& itemKey) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAttachmentColumns + " FROM attachments WHERE parentKey = ? ORDER BY key");
        st.bind(1, itemKey);
        return readAttachments(st);
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
    "coalesce(rs.progress, -1), i.itemType, i.tags, coalesce(m.read, -1), coalesce(m.tag, '')";
const char* kRowJoins =
    " LEFT JOIN attachments a ON a.key = i.readableKey LEFT JOIN reading_state rs ON rs.itemKey = i.key "
    "LEFT JOIN read_marks m ON m.itemKey = i.key ";

// Read: a mark made on the Kindle for the read tag (still to be sent) wins over the item's tags.
bool isReadNow(const std::vector<std::string>& tags, bool marked, bool markRead, const std::string& markTag,
               const std::string& readTag) {
    if (marked && markTag == readTag) return markRead;
    return std::find(tags.begin(), tags.end(), readTag) != tags.end();
}

std::vector<ItemRow> readRows(Stmt& st, const std::string& readTag) {
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
        r.itemType = st.text(8);
        r.read = isReadNow(strings(st.text(9)), st.integer(10) >= 0, st.integer(10) == 1, st.text(11), readTag);
        out.push_back(std::move(r));
    }
    return out;
}

// WHERE clause and bindings shared by rows() and countRows().
struct Where {
    std::string sql;
    std::vector<std::string> args;
};

Where whereOf(const ListQuery& q, const std::string& readTag) {
    Where w{" WHERE i.parentKey = ''" + notTrashed("i.key"), {}};
    switch (q.type) {
        case TypeFilter::WithFile: w.sql += " AND i.readableType != ''"; break;
        case TypeFilter::Epub: w.sql += " AND i.readableType = 'EPUB'"; break;
        case TypeFilter::Pdf: w.sql += " AND i.readableType = 'PDF'"; break;
        case TypeFilter::Any: break;
    }
    if (q.read != ReadFilter::Any) {  // as isReadNow()
        w.sql += std::string(q.read == ReadFilter::Unread ? " AND NOT" : " AND") +
                 " coalesce((SELECT r.read FROM read_marks r WHERE r.itemKey = i.key AND r.tag = ?), "
                 "EXISTS (SELECT 1 FROM json_each(i.tags) WHERE value = ?))";
        w.args.push_back(readTag);
        w.args.push_back(readTag);
    }
    if (q.downloaded)
        w.sql += " AND i.readableKey IN (SELECT key FROM attachments WHERE state = " +
                 std::to_string(static_cast<int>(AttachmentRecord::State::Local)) + ")";
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
        case SortBy::Title: return " ORDER BY i.titleKey, i.key";
        case SortBy::Author: return " ORDER BY i.authorKey, i.titleKey, i.key";
        case SortBy::Year: return " ORDER BY i.year DESC, i.titleKey, i.key";
        case SortBy::Recent: case SortBy::Added: break;
    }
    return " ORDER BY i.dateAdded DESC, i.key";
}

// What sectionCounts() groups by, in orderOf()'s order; "" when the sort has no sections.
std::string sectionOf(SortBy s) {
    switch (s) {
        case SortBy::Title: return "substr(i.titleKey, 1, 2) ORDER BY 1";
        case SortBy::Author: return "substr(i.authorKey, 1, 2) ORDER BY 1";
        case SortBy::Year: return "i.year ORDER BY 1 DESC";
        case SortBy::Added: return "substr(i.dateAdded, 1, 7) ORDER BY 1 DESC";
        case SortBy::Recent: break;
    }
    return "";
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
        Where w = whereOf(q, readTag());
        Stmt st = db_.prepare(std::string("SELECT ") + kRowColumns + " FROM items i" + kRowJoins + w.sql + orderOf(q.sort) +
                              " LIMIT ? OFFSET ?");
        int n = 1;
        for (const auto& a : w.args) st.bind(n++, a);
        st.bind(n, int64_t{q.limit});
        st.bind(n + 1, int64_t{q.offset});
        return readRows(st, readTag());
    });
}

int64_t Library::countRows(const ListQuery& q) {
    return guarded([&] {
        Where w = whereOf(q, readTag());
        Stmt st = db_.prepare("SELECT count(*) FROM items i" + w.sql);
        int n = 1;
        for (const auto& a : w.args) st.bind(n++, a);
        st.step();
        return st.integer(0);
    });
}

std::vector<SectionCount> Library::sectionCounts(const ListQuery& q) {
    const std::string by = sectionOf(q.sort);
    if (by.empty()) return {};
    return guarded([&] {
        Where w = whereOf(q, readTag());
        const std::string group = by.substr(0, by.find(" ORDER BY"));
        Stmt st = db_.prepare("SELECT " + group + ", count(*) FROM items i" + w.sql + " GROUP BY 1" + by.substr(group.size()));
        int n = 1;
        for (const auto& a : w.args) st.bind(n++, a);
        std::vector<SectionCount> out;
        while (st.step()) out.push_back({st.text(0), st.integer(1)});
        return out;
    });
}

std::vector<ItemRow> Library::searchRows(const std::string& query, int offset, int limit, const std::string& collectionKey) {
    const std::string fts = ftsQuery(query);
    if (fts.empty()) return {};
    return guarded([&] {
        // In a collection: its items and those of its subcollections, at any depth.
        const std::string inCollection =
            collectionKey.empty()
                ? ""
                : " AND i.key IN (SELECT itemKey FROM item_collections WHERE collectionKey IN ("
                  "WITH RECURSIVE sub(key) AS (SELECT ?4 UNION SELECT c.key FROM collections c JOIN sub ON c.parentKey = sub.key) "
                  "SELECT key FROM sub))";
        Stmt st = db_.prepare(std::string("SELECT ") + kRowColumns + " FROM item_fts JOIN items i ON i.id = item_fts.rowid" +
                              kRowJoins + " WHERE item_fts MATCH ?1 AND i.parentKey = ''" + notTrashed("i.key") + inCollection +
                              " ORDER BY rank LIMIT ?2 OFFSET ?3");
        st.bind(1, fts).bind(2, int64_t{limit}).bind(3, int64_t{offset});
        if (!collectionKey.empty()) st.bind(4, collectionKey);
        return readRows(st, readTag());
    });
}

std::vector<std::string> Library::recentSearches() {
    return guarded([&] { return strings(meta("recentSearches")); });
}

void Library::rememberSearch(const std::string& query) {
    if (query.find_first_not_of(" \t") == std::string::npos) return;
    guarded([&] {
        std::vector<std::string> list = strings(meta("recentSearches"));
        list.erase(std::remove(list.begin(), list.end(), query), list.end());
        list.insert(list.begin(), query);
        if (list.size() > kRecentSearches) list.resize(kRecentSearches);
        setMeta("recentSearches", json(list).dump());
    });
}

void Library::clearRecentSearches() {
    guarded([&] { setMeta("recentSearches", "[]"); });
}

std::optional<ItemRow> Library::rowFor(const std::string& itemKey) {
    return guarded([&]() -> std::optional<ItemRow> {
        Stmt st = db_.prepare(std::string("SELECT ") + kRowColumns + " FROM items i" + kRowJoins + " WHERE i.key = ?" +
                              notTrashed("i.key"));
        st.bind(1, itemKey);
        auto rows = readRows(st, readTag());
        if (rows.empty()) return std::nullopt;
        return rows[0];
    });
}

std::optional<ItemDetails> Library::details(const std::string& itemKey) {
    std::optional<ItemRecord> item = this->item(itemKey);
    if (!item) return std::nullopt;
    ItemDetails d;
    d.item = std::move(*item);
    if (auto j = json::parse(d.item.json, nullptr, false); j.is_object() && j.contains("abstractNote") &&
                                                            j["abstractNote"].is_string())
        d.abstract = j["abstractNote"].get<std::string>();
    // Collection paths, parent first.
    std::map<std::string, CollectionRecord> all;
    for (auto& c : collections()) all[c.key] = c;
    for (const auto& key : d.item.collections) {
        std::string path;
        for (auto it = all.find(key); it != all.end(); it = all.find(it->second.parentKey)) {
            path = path.empty() ? it->second.name : it->second.name + " › " + path;
            if (it->second.parentKey.empty()) break;
        }
        if (!path.empty()) d.collectionPaths.push_back(path);
    }
    std::sort(d.collectionPaths.begin(), d.collectionPaths.end());
    // A lone file is its own attachment.
    std::vector<AttachmentRecord> atts = attachmentsOf(itemKey);
    if (auto self = attachment(itemKey)) atts.insert(atts.begin(), *self);
    auto rank = [](const AttachmentRecord& a) {
        if (a.linkMode == "linked_file") return 3;
        return a.contentType == "application/epub+zip" ? 0 : a.contentType == "application/pdf" ? 1 : 2;
    };
    std::stable_sort(atts.begin(), atts.end(), [&](const auto& a, const auto& b) { return rank(a) < rank(b); });
    for (auto& a : atts) {
        ItemDetails::File f;
        std::error_code ec;
        f.onKindle = a.state == AttachmentRecord::State::Local && !a.localPath.empty() &&
                     std::filesystem::exists(a.localPath, ec);
        if (f.onKindle) f.bytes = static_cast<int64_t>(std::filesystem::file_size(a.localPath, ec));
        f.highlights = static_cast<int>(annotationsFor(a.key).size());
        f.attachment = std::move(a);
        d.files.push_back(std::move(f));
    }
    if (auto rs = readingState(itemKey); rs && rs->openedMs > 0) d.progress = rs->progress;
    d.read = isRead(itemKey);
    return d;
}

namespace {
ReadingPosition readPosition(const Stmt& st) {
    ReadingPosition p;
    p.attachmentKey = st.text(0);
    p.remote = st.text(1);
    p.remoteVersion = st.integer(2);
    p.local = st.text(3);
    p.dirty = st.integer(4) != 0;
    p.remoteNew = st.integer(5) != 0;
    p.lastSent = st.text(6);
    return p;
}
constexpr const char* kPositionColumns = "attachmentKey, remote, remoteVersion, local, dirty, remoteNew, lastSent";
}  // namespace

std::optional<ReadingPosition> Library::readingPosition(const std::string& key) {
    return guarded([&]() -> std::optional<ReadingPosition> {
        Stmt st = db_.prepare(std::string("SELECT ") + kPositionColumns + " FROM reading_positions WHERE attachmentKey = ?");
        st.bind(1, key);
        if (!st.step()) return std::nullopt;
        return readPosition(st);
    });
}

void Library::setLocalPosition(const std::string& key, const std::string& value) {
    guarded([&] {
        // To send whenever it differs from what Zotero has, unless the reader still has to ask.
        Stmt st = db_.prepare(
            "INSERT INTO reading_positions (attachmentKey, local, dirty) VALUES (?1, ?2, 1) "
            "ON CONFLICT(attachmentKey) DO UPDATE SET local = ?2, dirty = (?2 != remote AND NOT remoteNew)");
        st.bind(1, key);
        st.bind(2, value);
        st.step();
    });
}

void Library::applyRemotePosition(const std::string& key, const std::string& value, int64_t version) {
    guarded([&] {
        // News only if the Kindle has its own place and it differs. Then the Kindle's place is not
        // sent until the reader asks (Stay sends it): it must not overwrite the newer one unseen.
        // Our own write coming back (no newer than what we sent) is not news.
        Stmt st = db_.prepare(
            "INSERT INTO reading_positions (attachmentKey, remote, remoteVersion) VALUES (?1, ?2, ?3) "
            "ON CONFLICT(attachmentKey) DO UPDATE SET remote = ?2, remoteVersion = ?3, "
            "remoteNew = (local != '' AND local != ?2 AND lastSent != ?2), "
            "dirty = CASE WHEN local != '' AND local != ?2 AND lastSent != ?2 THEN 0 ELSE dirty AND local != ?2 END "
            "WHERE remoteVersion < ?3");
        st.bind(1, key);
        st.bind(2, value);
        st.bind(3, version);
        st.step();
    });
}

void Library::markPositionSending(const std::string& key, const std::string& value) {
    guarded([&] {
        Stmt st = db_.prepare("UPDATE reading_positions SET lastSent = ?2 WHERE attachmentKey = ?1");
        st.bind(1, key);
        st.bind(2, value);
        st.step();
    });
}

void Library::holdRemotePosition(const std::string& key) {
    guarded([&] {
        Stmt st = db_.prepare("UPDATE reading_positions SET remoteNew = 1, dirty = 0 WHERE attachmentKey = ? AND remote != ''");
        st.bind(1, key);
        st.step();
    });
}

void Library::markPositionSent(const std::string& key, const std::string& value, int64_t version) {
    guarded([&] {
        Stmt st = db_.prepare(
            "UPDATE reading_positions SET remote = ?2, remoteVersion = ?3, remoteNew = 0, dirty = (local != ?2), "
            "lastSent = ?2 WHERE attachmentKey = ?1");
        st.bind(1, key);
        st.bind(2, value);
        st.bind(3, version);
        st.step();
    });
}

void Library::answerRemotePosition(const std::string& key, bool go, int64_t version) {
    guarded([&] {
        Stmt st = db_.prepare(go ? "UPDATE reading_positions SET local = remote, dirty = 0, remoteNew = 0 "
                                   "WHERE attachmentKey = ?1 AND remoteVersion = ?2"
                                 : "UPDATE reading_positions SET dirty = (local != remote), remoteNew = 0 "
                                   "WHERE attachmentKey = ?1 AND remoteVersion = ?2");
        st.bind(1, key);
        st.bind(2, version);
        st.step();
    });
}

std::vector<ReadingPosition> Library::dirtyPositions() {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kPositionColumns + " FROM reading_positions WHERE dirty = 1");
        std::vector<ReadingPosition> out;
        while (st.step()) out.push_back(readPosition(st));
        return out;
    });
}

std::vector<Bookmark> Library::bookmarks(const std::string& key) {
    std::vector<Bookmark> out;
    const auto j = json::parse(metaValue("bookmarks:" + key), nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& b : j)
        if (b.is_object() && b.contains("pos") && b["pos"].is_string())
            out.push_back({b["pos"].get<std::string>(), b.value("chapter", ""), b.value("ms", int64_t{0})});
    return out;
}

namespace {
std::string bookmarksJson(const std::vector<Bookmark>& marks) {
    json j = json::array();
    for (const auto& b : marks) j.push_back({{"pos", b.position}, {"chapter", b.chapter}, {"ms", b.createdMs}});
    return j.dump();
}
}  // namespace

void Library::addBookmark(const std::string& key, const Bookmark& b) {
    auto marks = bookmarks(key);
    for (const auto& m : marks)
        if (m.position == b.position) return;
    marks.push_back(b);
    setMetaValue("bookmarks:" + key, bookmarksJson(marks));
}

void Library::removeBookmark(const std::string& key, const std::string& position) {
    auto marks = bookmarks(key);
    marks.erase(std::remove_if(marks.begin(), marks.end(), [&](const Bookmark& m) { return m.position == position; }),
                marks.end());
    setMetaValue("bookmarks:" + key, bookmarksJson(marks));
}

int64_t Library::settingsVersion() {
    const std::string v = metaValue("settings.version");
    return v.empty() ? 0 : std::stoll(v);
}

void Library::setSettingsVersion(int64_t v) { setMetaValue("settings.version", std::to_string(v)); }

void Library::removeDownload(const std::string& attachmentKey) {
    auto a = attachment(attachmentKey);
    if (!a || a->localPath.empty()) return;
    deleteDownloadedCopy(attachmentKey, a->localPath);  // <files>/<attachmentKey>/<name>, folder too
    setFileState(attachmentKey, AttachmentRecord::State::Remote, "");
}

std::vector<std::pair<std::string, int64_t>> Library::topTags(int n) {
    return guarded([&] {
        Stmt st = db_.prepare(
            "SELECT t.value, count(*) FROM items i, json_each(i.tags) t WHERE i.parentKey = ''" + notTrashed("i.key") +
            " GROUP BY t.value ORDER BY 2 DESC, 1 LIMIT ?");
        st.bind(1, int64_t{n});
        std::vector<std::pair<std::string, int64_t>> out;
        while (st.step()) out.emplace_back(st.text(0), st.integer(1));
        return out;
    });
}

namespace {
void markKept(std::vector<CollectionEntry>& entries, const std::vector<std::string>& kept) {
    for (auto& e : entries) e.kept = std::find(kept.begin(), kept.end(), e.collection.key) != kept.end();
}
}  // namespace

std::vector<CollectionEntry> Library::childCollections(const std::string& parentKey) {
    return guarded([&] {
        Stmt st = db_.prepare(
            "SELECT c.key, c.name, c.parentKey, c.version, "
            "(SELECT count(*) FROM item_collections ic WHERE ic.collectionKey = c.key" + notTrashed("ic.itemKey") + "), "
            "EXISTS (SELECT 1 FROM collections d WHERE d.parentKey = c.key) "
            "FROM collections c WHERE c.parentKey = ? ORDER BY c.name COLLATE NOCASE");
        st.bind(1, parentKey);
        std::vector<CollectionEntry> out;
        while (st.step())
            out.push_back({{st.text(0), st.text(1), st.text(2), st.integer(3)}, st.integer(4), st.integer(5) != 0});
        markKept(out, keptCollections());
        return out;
    });
}

std::vector<std::string> Library::pinnedCollections() {
    std::vector<std::string> out;
    const auto j = json::parse(metaValue("pinnedCollections"), nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& k : j)
        if (k.is_string()) out.push_back(k.get<std::string>());
    return out;
}

void Library::setCollectionPinned(const std::string& key, bool pinned) {
    auto keys = pinnedCollections();
    const bool was = std::find(keys.begin(), keys.end(), key) != keys.end();
    if (was == pinned) return;  // already so: keeps its place
    if (pinned) keys.push_back(key);
    else keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    setMetaValue("pinnedCollections", json(keys).dump());
}

std::vector<std::string> Library::keptCollections() {
    std::vector<std::string> out;
    const auto j = json::parse(metaValue("keptCollections"), nullptr, false);
    if (!j.is_array()) return out;
    for (const auto& k : j)
        if (k.is_string()) out.push_back(k.get<std::string>());
    return out;
}

void Library::setCollectionKept(const std::string& key, bool kept) {
    auto keys = keptCollections();
    const bool was = std::find(keys.begin(), keys.end(), key) != keys.end();
    if (was == kept) return;
    if (kept) keys.push_back(key);
    else keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
    setMetaValue("keptCollections", json(keys).dump());
}

std::vector<ItemRow> Library::collectionFiles(const std::string& collectionKey) {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kRowColumns + " FROM items i" + kRowJoins +
                              " WHERE i.parentKey = ''" + notTrashed("i.key") +
                              " AND i.readableKey != '' AND i.key IN (SELECT itemKey FROM item_collections WHERE collectionKey IN ("
                              "WITH RECURSIVE sub(key) AS (SELECT ?1 UNION SELECT c.key FROM collections c JOIN sub ON c.parentKey = sub.key) "
                              "SELECT key FROM sub)) ORDER BY i.title COLLATE NOCASE, i.key");
        st.bind(1, collectionKey);
        return readRows(st, readTag());
    });
}

std::vector<CollectionEntry> Library::collectionsTab() {
    const auto all = allCollections();
    auto find = [&](const std::string& key) -> const CollectionEntry* {
        for (const auto& e : all)
            if (e.collection.key == key) return &e;
        return nullptr;
    };
    std::vector<CollectionEntry> out;
    const auto pins = pinnedCollections();
    for (const auto& key : pins)
        if (const CollectionEntry* e = find(key)) {
            CollectionEntry p = *e;
            p.pinned = true;
            if (const CollectionEntry* parent = find(p.collection.parentKey)) p.parentName = parent->collection.name;
            out.push_back(std::move(p));
        }
    for (const auto& e : all)
        if (e.collection.parentKey.empty() && std::find(pins.begin(), pins.end(), e.collection.key) == pins.end())
            out.push_back(e);
    return out;
}

std::vector<CollectionEntry> Library::allCollections() {
    return guarded([&] {
        Stmt st = db_.prepare(
            "SELECT c.key, c.name, c.parentKey, c.version, "
            "(SELECT count(*) FROM item_collections ic WHERE ic.collectionKey = c.key" + notTrashed("ic.itemKey") + "), "
            "EXISTS (SELECT 1 FROM collections d WHERE d.parentKey = c.key) "
            "FROM collections c ORDER BY c.name COLLATE NOCASE");
        std::vector<CollectionEntry> out;
        while (st.step())
            out.push_back({{st.text(0), st.text(1), st.text(2), st.integer(3)}, st.integer(4), st.integer(5) != 0});
        markKept(out, keptCollections());
        return out;
    });
}

std::vector<ItemRow> Library::recentlyOpened(int limit) {
    std::vector<std::string> keys = guarded([&] {
        Stmt st = db_.prepare("SELECT itemKey FROM reading_state WHERE openedMs > 0 ORDER BY openedMs DESC LIMIT ?");
        st.bind(1, int64_t{limit});
        std::vector<std::string> out;
        while (st.step()) out.push_back(st.text(0));
        return out;
    });
    std::vector<ItemRow> rows;
    for (const auto& k : keys)
        if (auto r = rowFor(k)) rows.push_back(std::move(*r));
    return rows;
}

std::vector<NoteRow> Library::recentAnnotations(int offset, int limit, const std::string& query) {
    const std::string fts = ftsQuery(query);
    if (!query.empty() && fts.empty()) return {};
    return guarded([&] {
        std::string cols;
        for (const char* c : {"key", "parentKey", "type", "text", "comment", "color", "pageLabel", "sortIndex", "position",
                              "tags", "version", "dirty", "conflict", "createdHere", "json", "dateModified", "deletedHere",
                              "localOnly", "conflictOf"})
            cols += std::string(cols.empty() ? "" : ", ") + "n." + c;
        Stmt st = db_.prepare("SELECT " + cols +
                              ", coalesce(i.key, ''), coalesce(i.title, '') FROM annotations n "
                              "LEFT JOIN attachments a ON a.key = n.parentKey "
                              "LEFT JOIN items i ON i.key = CASE WHEN a.parentKey = '' THEN a.key ELSE a.parentKey END "
                              "WHERE n.deletedHere = 0" +
                              (fts.empty() ? std::string(" ORDER BY n.dateModified DESC, n.key")
                                           : std::string(" AND n.rowid IN (SELECT rowid FROM note_fts WHERE note_fts MATCH ?3) "
                                                         "ORDER BY i.title COLLATE NOCASE, i.key, n.parentKey, n.sortIndex, n.key")) +
                              " LIMIT ?1 OFFSET ?2");
        st.bind(1, int64_t{limit}).bind(2, int64_t{offset});
        if (!fts.empty()) st.bind(3, fts);
        std::vector<NoteRow> out;
        while (st.step()) {
            NoteRow row;
            row.annotation = readAnnotation(st);
            row.itemKey = st.text(19);
            row.itemTitle = st.text(20);
            out.push_back(std::move(row));
        }
        return out;
    });
}

int64_t Library::countAnnotations(const std::string& query) {
    const std::string fts = ftsQuery(query);
    if (!query.empty() && fts.empty()) return 0;
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT count(*) FROM annotations WHERE deletedHere = 0") +
                              (fts.empty() ? "" : " AND rowid IN (SELECT rowid FROM note_fts WHERE note_fts MATCH ?)"));
        if (!fts.empty()) st.bind(1, fts);
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
            "SELECT itemKey, attachmentKey, openedMs, progress, location FROM reading_state WHERE openedMs > 0 "
            "ORDER BY openedMs DESC LIMIT 1");
        if (!st.step()) return std::nullopt;
        return ReadingState{st.text(0), st.text(1), st.integer(2), st.real(3), st.text(4)};
    });
}

std::string Library::readTag() {
    return guarded([&] {
        const std::string t = meta("readTag");
        return t.empty() ? std::string(kDefaultReadTag) : t;
    });
}

void Library::setReadTag(const std::string& tag) {
    guarded([&] { setMeta("readTag", tag.empty() ? std::string(kDefaultReadTag) : tag); });
}

bool Library::isRead(const std::string& itemKey) {
    return guarded([&] {
        const std::string tag = readTag();
        Stmt st = db_.prepare(
            "SELECT i.tags, coalesce(m.read, -1), coalesce(m.tag, '') FROM items i "
            "LEFT JOIN read_marks m ON m.itemKey = i.key WHERE i.key = ?");
        st.bind(1, itemKey);
        if (!st.step()) return false;
        return isReadNow(strings(st.text(0)), st.integer(1) >= 0, st.integer(1) == 1, st.text(2), tag);
    });
}

void Library::markRead(const std::string& itemKey, bool read) {
    guarded([&] {
        Stmt st = db_.prepare(
            "INSERT INTO read_marks (itemKey, read, tag) VALUES (?, ?, ?) ON CONFLICT(itemKey) DO UPDATE "
            "SET read = excluded.read, tag = excluded.tag");
        st.bind(1, itemKey).bind(2, int64_t{read}).bind(3, readTag()).step();
    });
}

std::vector<ReadMark> Library::readMarks() {
    return guarded([&] {
        std::vector<ReadMark> out;
        Stmt st = db_.prepare("SELECT itemKey, read, tag FROM read_marks ORDER BY itemKey");
        while (st.step()) out.push_back({st.text(0), st.integer(1) != 0, st.text(2)});
        return out;
    });
}

void Library::clearReadMark(const ReadMark& m) {
    guarded([&] {
        Stmt st = db_.prepare("DELETE FROM read_marks WHERE itemKey = ? AND read = ? AND tag = ?");
        st.bind(1, m.itemKey).bind(2, int64_t{m.read}).bind(3, m.tag).step();
    });
}

std::optional<ItemRecord> Library::item(const std::string& key) {
    return guarded([&]() -> std::optional<ItemRecord> {
        Stmt st = db_.prepare(std::string("SELECT ") + kItemColumns + " FROM items WHERE key = ?" + notTrashed("items.key"));
        st.bind(1, key);
        if (!st.step()) return std::nullopt;
        return readItem(st);
    });
}

std::optional<ItemRecord> Library::zoteroItem(const std::string& key) {
    return guarded([&]() -> std::optional<ItemRecord> {
        Stmt st = db_.prepare(std::string("SELECT ") + kItemColumns + " FROM items WHERE key = ?");
        st.bind(1, key);
        if (!st.step()) return std::nullopt;
        ItemRecord r = readItem(st);
        baseLists(r.json, r.tags, r.collections);  // without pending changes (when it holds them)
        return r;
    });
}

std::optional<ReadingState> Library::readingState(const std::string& itemKey) {
    return guarded([&]() -> std::optional<ReadingState> {
        Stmt st = db_.prepare("SELECT itemKey, attachmentKey, openedMs, progress, location FROM reading_state WHERE itemKey = ?");
        st.bind(1, itemKey);
        if (!st.step()) return std::nullopt;
        return ReadingState{st.text(0), st.text(1), st.integer(2), st.real(3), st.text(4)};
    });
}

void Library::saveReading(const std::string& itemKey, const std::string& location, double progress) {
    guarded([&] {
        Stmt st = db_.prepare(
            "INSERT INTO reading_state (itemKey, location, progress) VALUES (?, ?, ?) ON CONFLICT(itemKey) DO UPDATE "
            "SET location = excluded.location, progress = excluded.progress");
        st.bind(1, itemKey).bind(2, location);
        st.bindReal(3, progress);
        st.step();
    });
}

void Library::setFileState(const std::string& attachmentKey, AttachmentRecord::State state, const std::string& localPath) {
    guarded([&] {
        Stmt st = db_.prepare("UPDATE attachments SET state = ?, localPath = ? WHERE key = ?");
        st.bind(1, static_cast<int64_t>(state)).bind(2, localPath).bind(3, attachmentKey).step();
    });
}

std::vector<std::string> Library::attachmentKeys() {
    return guarded([&] {
        Stmt st = db_.prepare("SELECT key FROM attachments");
        std::vector<std::string> out;
        while (st.step()) out.push_back(st.text(0));
        return out;
    });
}

std::vector<AttachmentRecord> Library::downloadedAttachments() {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAttachmentColumns + " FROM attachments WHERE state = ? ORDER BY key");
        st.bind(1, static_cast<int64_t>(AttachmentRecord::State::Local));
        return readAttachments(st);
    });
}

std::string newLocalKey() {
    static std::mt19937 rng{std::random_device{}()};
    static const char* alphabet = "23456789ABCDEFGHIJKLMNPQRSTUVWXYZ";
    std::string k(8, '2');
    for (auto& c : k) c = alphabet[rng() % 33];
    return k;
}

bool isZoteroKey(const std::string& k) {
    return k.size() == 8 && k.find_first_not_of("23456789ABCDEFGHIJKLMNPQRSTUVWXYZ") == std::string::npos;
}

std::string Library::createAnnotation(AnnotationRecord a) {
    do a.key = newLocalKey();
    while (annotation(a.key) || objectVersion(a.key) > 0);
    a.version = 0;
    a.createdHere = true;
    a.dirty = true;
    a.conflict = a.deletedHere = false;
    a.conflictOf.clear();
    a.dateModified = nowUtc();
    upsertAnnotation(a);
    return a.key;
}

void Library::editAnnotation(const AnnotationRecord& edited) {
    auto old = annotation(edited.key);
    if (!old || old->deletedHere) return;
    AnnotationRecord a = edited;
    a.version = old->version;
    a.createdHere = old->createdHere;
    a.conflict = old->conflict;
    a.conflictOf = old->conflictOf;
    a.localOnly = old->localOnly;
    a.deletedHere = false;
    a.dirty = !old->conflict;  // a conflict copy is only sent once re-applied
    a.dateModified = nowUtc();
    upsertAnnotation(a);
}

namespace {
constexpr const char* kUploadsSent = "annotations.uploadsSent";  // keys, one per line
}  // namespace

void Library::markUploadSent(const std::vector<std::string>& keys) {
    std::string kept;
    std::istringstream in(metaValue(kUploadsSent));
    for (std::string k; std::getline(in, k);)  // only those still waiting for a version
        if (auto a = annotation(k); a && a->version == 0) kept += k + "\n";
    for (const auto& k : keys)
        if (kept.find(k + "\n") == std::string::npos) kept += k + "\n";
    setMetaValue(kUploadsSent, kept);
}

bool Library::uploadSent(const std::string& key) {
    return ("\n" + metaValue(kUploadsSent)).find("\n" + key + "\n") != std::string::npos;
}

void Library::deleteAnnotation(const std::string& key) {
    auto a = annotation(key);
    if (!a) return;
    if (a->version == 0 && !uploadSent(key)) {  // never left the Kindle
        removeObject("annotation", key);
        return;
    }
    a->deletedHere = true;
    a->dirty = true;
    upsertAnnotation(*a);
}

std::vector<AnnotationRecord> Library::pendingDeletions() {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAnnotationColumns +
                              " FROM annotations WHERE deletedHere = 1 ORDER BY key");
        return readAnnotations(st);
    });
}

std::vector<AnnotationRecord> Library::conflicts() {
    return guarded([&] {
        Stmt st = db_.prepare(std::string("SELECT ") + kAnnotationColumns +
                              " FROM annotations WHERE conflict = 1 ORDER BY dateModified DESC, key");
        return readAnnotations(st);
    });
}

void Library::discardConflict(const std::string& key) {
    auto a = annotation(key);
    if (a && a->conflict) removeObject("annotation", key);
}

std::string Library::reapplyConflict(const std::string& key) {
    auto copy = annotation(key);
    if (!copy || !copy->conflict) return {};
    auto tx = begin();
    removeObject("annotation", key);
    std::string result;
    auto original = copy->conflictOf.empty() ? std::nullopt : annotation(copy->conflictOf);
    if (original && !original->deletedHere) {
        AnnotationRecord a = *copy;
        a.key = original->key;
        editAnnotation(a);  // keeps the original's version, so the server's check still applies
        result = a.key;
    } else {
        result = createAnnotation(*copy);
    }
    tx.commit();
    return result;
}

// ---- Plugin changes ------------------------------------------------------------------------

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string str(const json& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string yearOfDate(const std::string& date) {
    for (size_t i = 0; i + 4 <= date.size(); ++i) {
        bool digits = true;
        for (size_t j = i; j < i + 4 && digits; ++j) digits = std::isdigit(static_cast<unsigned char>(date[j])) != 0;
        const bool alone = (i == 0 || !std::isdigit(static_cast<unsigned char>(date[i - 1]))) &&
                           (i + 4 == date.size() || !std::isdigit(static_cast<unsigned char>(date[i + 4])));
        if (digits && alone) return date.substr(i, 4);
    }
    return "";
}

bool among(const std::string& s, std::initializer_list<const char*> set) {
    for (const char* x : set)
        if (s == x) return true;
    return false;
}

// zk.zotero.createItem's ItemFields (zk.d.ts) as Zotero item data, with its collections and tags.
// A field Zotero has no place for in that item type goes into `extra`, as Zotero itself writes
// such fields ("DOI: …", "arXiv: …"). nullopt if the type or title is missing.
std::optional<json> zoteroDataOf(const json& fields, const std::vector<std::string>& collections,
                                 const std::vector<std::string>& tags) {
    if (!fields.is_object()) return std::nullopt;
    const std::string type = str(fields, "type"), title = str(fields, "title");
    if (type.empty() || title.empty()) return std::nullopt;
    json d = json::object();
    d["itemType"] = type;
    d["title"] = title;
    json creators = json::array();
    if (auto it = fields.find("creators"); it != fields.end() && it->is_array())
        for (const auto& c : *it) {
            if (!c.is_object()) continue;
            json z = {{"creatorType", str(c, "type").empty() ? std::string("author") : str(c, "type")}};
            if (str(c, "first").empty() && str(c, "last").empty()) {
                z["name"] = str(c, "name");
            } else {
                z["firstName"] = str(c, "first");
                z["lastName"] = str(c, "last");
            }
            creators.push_back(std::move(z));
        }
    d["creators"] = std::move(creators);
    std::string extra = str(fields, "extra");
    auto extraLine = [&](const std::string& line) { extra += (extra.empty() ? "" : "\n") + line; };
    if (const std::string v = str(fields, "date"); !v.empty()) d["date"] = v;
    if (const std::string v = str(fields, "url"); !v.empty()) d["url"] = v;
    if (const std::string v = str(fields, "abstract"); !v.empty()) d["abstractNote"] = v;
    if (const std::string v = str(fields, "publication"); !v.empty()) {
        static const std::map<std::string, const char*> field = {
            {"journalArticle", "publicationTitle"}, {"magazineArticle", "publicationTitle"},
            {"newspaperArticle", "publicationTitle"}, {"bookSection", "bookTitle"},
            {"conferencePaper", "proceedingsTitle"}, {"webpage", "websiteTitle"}, {"blogPost", "blogTitle"},
            {"preprint", "repository"}, {"encyclopediaArticle", "encyclopediaTitle"},
            {"dictionaryEntry", "dictionaryTitle"}};
        if (auto it = field.find(type); it != field.end()) d[it->second] = v;
        else extraLine("Publication: " + v);
    }
    if (const std::string v = str(fields, "doi"); !v.empty()) {
        if (among(type, {"journalArticle", "conferencePaper", "preprint", "dataset", "standard"})) d["DOI"] = v;
        else extraLine("DOI: " + v);
    }
    if (const std::string v = str(fields, "isbn"); !v.empty()) {
        if (among(type, {"book", "bookSection", "conferencePaper", "encyclopediaArticle", "dictionaryEntry"})) d["ISBN"] = v;
        else extraLine("ISBN: " + v);
    }
    if (const std::string v = str(fields, "arxiv"); !v.empty()) {
        if (type == "preprint") d["archiveID"] = "arXiv:" + v;
        else extraLine("arXiv: " + v);
    }
    if (!extra.empty()) d["extra"] = extra;
    d["collections"] = collections;
    d["tags"] = json::array();
    for (const auto& t : tags) d["tags"].push_back({{"tag", t}});
    return d;
}

std::vector<std::string> uniqueStrings(const json& o, const char* key) {
    std::vector<std::string> out;
    if (auto it = o.find(key); it != o.end() && it->is_array())
        for (const auto& v : *it)
            if (v.is_string() && !v.get<std::string>().empty() &&
                std::find(out.begin(), out.end(), v.get<std::string>()) == out.end())
                out.push_back(v.get<std::string>());
    return out;
}

bool isListChange(PluginChange::Kind k) {
    using K = PluginChange::Kind;
    return k == K::AddTag || k == K::RemoveTag || k == K::AddToCollection || k == K::RemoveFromCollection;
}

// One tag or collection change made to lists of tags and collections.
void applyListChange(PluginChange::Kind k, const std::string& value, std::vector<std::string>& tags,
                     std::vector<std::string>& collections) {
    using K = PluginChange::Kind;
    auto& list = k == K::AddTag || k == K::RemoveTag ? tags : collections;
    const bool add = k == K::AddTag || k == K::AddToCollection;
    const bool there = std::find(list.begin(), list.end(), value) != list.end();
    if (add && !there) list.push_back(value);
    if (!add) list.erase(std::remove(list.begin(), list.end(), value), list.end());
}

}  // namespace

template <class F> void Library::atomically(F&& f) {
    if (inTx_) return f();
    auto tx = begin();
    f();
    tx.commit();
}

std::string Library::freshKey() {
    for (;;) {
        const std::string k = newLocalKey();
        Stmt st = db_.prepare(
            "SELECT EXISTS (SELECT 1 FROM items WHERE key = ?1) OR EXISTS (SELECT 1 FROM notes WHERE key = ?1) "
            "OR EXISTS (SELECT 1 FROM attachments WHERE key = ?1) OR EXISTS (SELECT 1 FROM annotations WHERE key = ?1) "
            "OR EXISTS (SELECT 1 FROM object_versions WHERE key = ?1)");
        st.bind(1, k);
        st.step();
        if (st.integer(0) == 0) return k;
    }
}

// The item's tags and collections as shown: Zotero's (in its json) with its pending changes made.
void Library::overlayPluginChanges(const std::string& itemKey) {
    Stmt row = db_.prepare("SELECT json, tags, collections FROM items WHERE key = ?");
    row.bind(1, itemKey);
    if (!row.step()) return;
    std::vector<std::string> tags, collections;
    if (!baseLists(row.text(0), tags, collections)) {
        tags = strings(row.text(1));
        collections = strings(row.text(2));
    }
    Stmt changes = db_.prepare("SELECT kind, value FROM plugin_changes WHERE itemKey = ? AND state = 'pending' ORDER BY seq");
    changes.bind(1, itemKey);
    while (changes.step()) {
        PluginChange::Kind k;
        if (kindFromName(changes.text(0), k) && isListChange(k)) applyListChange(k, changes.text(1), tags, collections);
    }
    Stmt set = db_.prepare("UPDATE items SET tags = ?, tagsText = ?, collections = ? WHERE key = ?");
    set.bind(1, json(tags).dump()).bind(2, joined(tags)).bind(3, json(collections).dump()).bind(4, itemKey).step();
    Stmt clear = db_.prepare("DELETE FROM item_collections WHERE itemKey = ?");
    clear.bind(1, itemKey).step();
    for (const auto& c : collections) {
        Stmt add = db_.prepare("INSERT OR IGNORE INTO item_collections (itemKey, collectionKey) VALUES (?, ?)");
        add.bind(1, itemKey).bind(2, c).step();
    }
    touch(itemKey);
}

std::string Library::queuePluginChange(PluginChange c, std::string* why) {
    using K = PluginChange::Kind;
    return guarded([&] {
        std::string id;
        auto refuse = [&](const std::string& reason) {
            if (why) *why = reason;
        };
        atomically([&] {
            auto exists = [&](const char* table, const std::string& key) {
                Stmt st = db_.prepare(std::string("SELECT 1 FROM ") + table + " WHERE key = ?");
                st.bind(1, key);
                return st.step();
            };
            auto trashPending = [&](const std::string& key) {
                Stmt st = db_.prepare("SELECT 1 FROM plugin_changes WHERE itemKey = ? AND kind = 'trash' AND state = 'pending'");
                st.bind(1, key);
                return st.step();
            };
            const bool pending = c.state == PluginChange::State::Pending;
            std::optional<ItemRecord> created;
            if (pending) {
                c.dependsOn.clear();
                const std::string target = c.kind == K::AddNote ? c.parentKey : c.itemKey;
                if (c.kind != K::CreateItem && c.kind != K::DeleteNote && trashPending(target))
                    return refuse("item " + target + " is being moved to the trash");
                switch (c.kind) {
                    case K::CreateItem: {
                        json v = json::parse(c.value, nullptr, false);
                        if (!v.is_object()) return refuse("the item's fields are not JSON");
                        const auto collections = uniqueStrings(v, "collections"), tags = uniqueStrings(v, "tags");
                        for (const auto& k : collections)
                            if (!exists("collections", k)) return refuse("no collection " + k);
                        auto data = zoteroDataOf(v.contains("fields") ? v["fields"] : json(), collections, tags);
                        if (!data) return refuse("an item needs a type and a title");
                        const json& f = v["fields"];
                        ItemRecord r;
                        r.key = c.itemKey = freshKey();
                        r.itemType = str(f, "type");
                        r.title = str(f, "title");
                        r.date = str(f, "date");
                        r.year = yearOfDate(r.date);
                        r.publication = str(f, "publication");
                        for (const auto& z : (*data)["creators"])
                            r.creators.push_back({str(z, "firstName"), str(z, "lastName"), str(z, "name"), str(z, "creatorType")});
                        r.collections = collections;
                        r.tags = tags;
                        r.dateAdded = r.dateModified = nowUtc();
                        r.json = data->dump();
                        created = std::move(r);
                        break;
                    }
                    case K::AddNote: {
                        Stmt st = db_.prepare("SELECT parentKey, itemType FROM items WHERE key = ?");
                        st.bind(1, c.parentKey);
                        if (!st.step()) return refuse("no item " + c.parentKey);
                        if (!st.text(0).empty() || st.text(1) == "attachment" || st.text(1) == "note")
                            return refuse("a note can only go on a regular item, not " + c.parentKey);
                        c.itemKey = freshKey();
                        break;
                    }
                    case K::DeleteNote:
                        if (!exists("notes", c.itemKey)) return refuse("no note " + c.itemKey);
                        break;
                    case K::Trash:
                        if (!exists("items", c.itemKey)) return refuse("no item " + c.itemKey);
                        break;
                    default:  // a tag or collection
                        if (c.value.empty())
                            return refuse(c.kind == K::AddTag || c.kind == K::RemoveTag ? "an empty tag" : "no collection given");
                        if (!exists("items", c.itemKey)) return refuse("no item " + c.itemKey);
                        if (c.kind == K::AddToCollection && !exists("collections", c.value))
                            return refuse("no collection " + c.value);
                        break;
                }
                // Targets what a pending change creates: sent after it, withdrawn with it.
                Stmt dep = db_.prepare(
                    "SELECT id FROM plugin_changes WHERE itemKey = ? AND state = 'pending' "
                    "AND kind IN ('createItem', 'addNote') ORDER BY seq DESC LIMIT 1");
                dep.bind(1, c.kind == K::AddNote ? c.parentKey : c.itemKey);
                if (c.kind != K::CreateItem && dep.step()) c.dependsOn = dep.text(0);
            }
            do c.id = newLocalKey() + newLocalKey().substr(0, 4);
            while (pluginChange(c.id));
            if (c.createdMs == 0) c.createdMs = nowMs();
            Stmt ins = db_.prepare(
                "INSERT INTO plugin_changes (id, seq, pluginId, kind, itemKey, value, parentKey, dependsOn, state, message, "
                "createdMs, sentMs) VALUES (?1, (SELECT coalesce(max(seq), 0) + 1 FROM plugin_changes), ?2, ?3, ?4, ?5, ?6, "
                "?7, ?8, ?9, ?10, ?11)");
            ins.bind(1, c.id).bind(2, c.pluginId).bind(3, std::string(kindName(c.kind))).bind(4, c.itemKey);
            ins.bind(5, c.value).bind(6, c.parentKey).bind(7, c.dependsOn).bind(8, std::string(stateName(c.state)));
            ins.bind(9, c.message).bind(10, c.createdMs).bind(11, c.sentMs).step();
            if (pending) {
                switch (c.kind) {
                    case K::CreateItem: upsertItem(*created); break;
                    case K::AddNote: {
                        NoteRecord n;
                        n.key = c.itemKey;
                        n.parentKey = c.parentKey;
                        n.html = c.value;
                        n.json = json{{"itemType", "note"}, {"parentItem", c.parentKey}, {"note", c.value}}.dump();
                        n.dateModified = nowUtc();
                        n.createdBy = c.pluginId;
                        upsertNote(n);
                        break;
                    }
                    case K::Trash:
                    case K::DeleteNote: break;  // hidden while pending (read-time)
                    default: {
                        // Zotero's lists kept in its json first, so the change can be taken back.
                        Stmt row = db_.prepare("SELECT json, tags, collections FROM items WHERE key = ?");
                        row.bind(1, c.itemKey);
                        if (row.step()) {
                            const std::string seeded = withBaseLists(row.text(0), strings(row.text(1)), strings(row.text(2)));
                            Stmt set = db_.prepare("UPDATE items SET json = ? WHERE key = ?");
                            set.bind(1, seeded).bind(2, c.itemKey).step();
                        }
                        overlayPluginChanges(c.itemKey);
                    }
                }
            }
            prunePluginLog(c.pluginId);
            id = c.id;
        });
        return id;
    });
}

std::optional<PluginChange> Library::pluginChange(const std::string& id) {
    return guarded([&]() -> std::optional<PluginChange> {
        Stmt st = db_.prepare(std::string("SELECT ") + kChangeColumns + " FROM plugin_changes WHERE id = ?");
        st.bind(1, id);
        if (!st.step()) return std::nullopt;
        return readChange(st);
    });
}

// Ends a pending change (and first what depends on it) and puts the local copy back.
void Library::withdrawPluginChange(const PluginChange& c, PluginChange::State state, const std::string& message,
                                   const std::string& dependentsMessage) {
    using K = PluginChange::Kind;
    std::vector<PluginChange> dependents;
    {
        Stmt st = db_.prepare(std::string("SELECT ") + kChangeColumns +
                              " FROM plugin_changes WHERE dependsOn = ? AND state = 'pending' ORDER BY seq DESC");
        st.bind(1, c.id);
        while (st.step()) dependents.push_back(readChange(st));
    }
    const bool refused = state == PluginChange::State::Refused;
    for (const auto& d : dependents)
        withdrawPluginChange(d, refused ? PluginChange::State::Refused : PluginChange::State::Cancelled,
                             !dependentsMessage.empty() ? dependentsMessage
                             : refused                  ? "Refused with what it was for."
                                                        : "Withdrawn with what it was for.",
                             dependentsMessage);
    Stmt set = db_.prepare("UPDATE plugin_changes SET state = ?, message = ? WHERE id = ?");
    set.bind(1, std::string(stateName(state))).bind(2, message).bind(3, c.id).step();
    switch (c.kind) {
        case K::CreateItem: removeObject("item", c.itemKey); break;
        case K::AddNote: removeObject("note", c.itemKey); break;
        case K::Trash:
        case K::DeleteNote: break;  // shown again: no longer pending
        default: overlayPluginChanges(c.itemKey);
    }
}

void Library::prunePluginLog(const std::string& pluginId) {
    Stmt st = db_.prepare(
        "DELETE FROM plugin_changes WHERE pluginId = ?1 AND state != 'pending' AND seq NOT IN (SELECT seq FROM "
        "plugin_changes WHERE pluginId = ?1 AND state != 'pending' ORDER BY seq DESC LIMIT ?2)");
    st.bind(1, pluginId).bind(2, int64_t{kPluginChangeLog}).step();
}

bool Library::cancelPluginChange(const std::string& pluginId, const std::string& changeId) {
    return guarded([&] {
        bool done = false;
        atomically([&] {
            auto c = pluginChange(changeId);
            if (!c || c->pluginId != pluginId || c->state != PluginChange::State::Pending) return;
            withdrawPluginChange(*c, PluginChange::State::Cancelled, "");
            prunePluginLog(pluginId);
            done = true;
        });
        return done;
    });
}

std::vector<PluginChange> Library::pendingPluginChanges(const std::string& pluginId) {
    return guarded([&] {
        std::vector<PluginChange> all;
        Stmt st = db_.prepare(std::string("SELECT ") + kChangeColumns + " FROM plugin_changes WHERE state = 'pending'" +
                              (pluginId.empty() ? "" : " AND pluginId = ?") + " ORDER BY seq");
        if (!pluginId.empty()) st.bind(1, pluginId);
        while (st.step()) all.push_back(readChange(st));
        // Creation order already puts a change after what it depends on (that existed first);
        // kept so explicitly, should a dependency ever be queued later.
        std::vector<PluginChange> out;
        std::set<std::string> placed, visiting;
        std::map<std::string, size_t> at;
        for (size_t i = 0; i < all.size(); ++i) at[all[i].id] = i;
        std::function<void(size_t)> place = [&](size_t i) {
            const PluginChange& c = all[i];
            if (placed.count(c.id) || !visiting.insert(c.id).second) return;
            if (auto d = at.find(c.dependsOn); !c.dependsOn.empty() && d != at.end()) place(d->second);
            placed.insert(c.id);
            out.push_back(c);
        };
        for (size_t i = 0; i < all.size(); ++i) place(i);
        return out;
    });
}

std::vector<PluginChange> Library::pluginChangeLog(const std::string& pluginId, int limit) {
    return guarded([&] {
        std::vector<PluginChange> out;
        Stmt st = db_.prepare(std::string("SELECT ") + kChangeColumns +
                              " FROM plugin_changes WHERE pluginId = ? ORDER BY seq DESC LIMIT ?");
        st.bind(1, pluginId).bind(2, int64_t{limit});
        while (st.step()) out.push_back(readChange(st));
        return out;
    });
}

void Library::markPluginChange(const std::string& id, PluginChange::State state, const std::string& message,
                               int64_t sentMs) {
    using K = PluginChange::Kind;
    using S = PluginChange::State;
    guarded([&] {
        atomically([&] {
            auto c = pluginChange(id);
            if (!c || c->state != S::Pending) return;
            switch (state) {
                case S::Pending: {
                    Stmt st = db_.prepare("UPDATE plugin_changes SET message = ? WHERE id = ?");
                    st.bind(1, message).bind(2, id).step();
                    return;
                }
                case S::Sent: {
                    Stmt st = db_.prepare("UPDATE plugin_changes SET state = 'sent', message = ?, sentMs = ? WHERE id = ?");
                    st.bind(1, message).bind(2, sentMs).bind(3, id).step();
                    if (isListChange(c->kind)) {
                        // Zotero has it now: part of its copy here (until its own copy comes in).
                        Stmt row = db_.prepare("SELECT json, tags, collections FROM items WHERE key = ?");
                        row.bind(1, c->itemKey);
                        if (row.step()) {
                            json d = json::parse(withBaseLists(row.text(0), strings(row.text(1)), strings(row.text(2))));
                            std::vector<std::string> tags, collections;
                            baseLists(d.dump(), tags, collections);
                            applyListChange(c->kind, c->value, tags, collections);
                            json zt = json::array();
                            for (const auto& t : tags) {  // keeps each tag's other fields (type)
                                json entry = {{"tag", t}};
                                for (const auto& old : d["tags"])
                                    if (old.is_object() && old.value("tag", "") == t) entry = old;
                                zt.push_back(entry);
                            }
                            d["tags"] = zt;
                            d["collections"] = collections;
                            Stmt set = db_.prepare("UPDATE items SET json = ? WHERE key = ?");
                            set.bind(1, d.dump()).bind(2, c->itemKey).step();
                        }
                        overlayPluginChanges(c->itemKey);
                    } else if (c->kind == K::Trash) {
                        removeObject("item", c->itemKey);  // in Zotero's trash: gone here, as a pull would
                    } else if (c->kind == K::DeleteNote) {
                        removeObject("note", c->itemKey);
                    }
                    break;
                }
                case S::Refused:
                case S::Cancelled:
                case S::Declined: withdrawPluginChange(*c, state, message); break;
            }
            prunePluginLog(c->pluginId);
        });
    });
}

int Library::discardPluginChanges(const std::string& pluginId) {
    return guarded([&] {
        int n = 0;
        atomically([&] {
            for (;;) {  // newest first; one at a time, as a withdrawal may take dependents with it
                Stmt st = db_.prepare(std::string("SELECT ") + kChangeColumns +
                                      " FROM plugin_changes WHERE pluginId = ? AND state = 'pending' ORDER BY seq DESC LIMIT 1");
                st.bind(1, pluginId);
                if (!st.step()) break;
                const PluginChange c = readChange(st);
                Stmt before = db_.prepare("SELECT count(*) FROM plugin_changes WHERE state = 'pending'");
                before.step();
                const int64_t was = before.integer(0);
                withdrawPluginChange(c, PluginChange::State::Cancelled, "");
                Stmt after = db_.prepare("SELECT count(*) FROM plugin_changes WHERE state = 'pending'");
                after.step();
                n += static_cast<int>(was - after.integer(0));
            }
            prunePluginLog(pluginId);
        });
        return n;
    });
}

}  // namespace zk
