#pragma once
// The local copy of the Zotero library: items, collections, attachments, annotations, and
// full-text indexes over item metadata and over annotation text. All SQL lives here.

#include "net/errors.h"
#include "store/db.h"
#include "store/plugin_changes.h"
#include "store/records.h"

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zk {

class Library;

// A fresh key in Zotero's alphabet (8 of 23456789ABCDEFGHIJKLMNPQRSTUVWXYZ) for a Kindle-made annotation.
std::string newLocalKey();
bool isZoteroKey(const std::string&);

// Recent and Added: newest added first (Added comes in months, Recent without sections).
enum class SortBy { Recent, Title, Author, Year, Added };
// WithFile: an EPUB, PDF or web snapshot the Kindle can open (Any: metadata-only items too). Pdf,
// Epub, Web: the file the item opens (ItemRow::badge) is one.
enum class TypeFilter { Any, WithFile, Pdf, Epub, Web };
// Unread / Read: as ItemRow::read.
enum class ReadFilter { Any, Unread, Read };

// The Zotero tag that means "read", unless the user picks another (Settings).
constexpr const char* kDefaultReadTag = "read";

struct ListQuery {
    SortBy sort = SortBy::Recent;
    TypeFilter type = TypeFilter::Any;
    ReadFilter read = ReadFilter::Any;
    bool downloaded = false;  // only items with a readable file on the Kindle (any of them)
    std::string tag, collectionKey;
    int64_t lib = kAllLibraries;  // the Library filter: kAllLibraries, kMyLibrary or a group ID
    int offset = 0, limit = 8;
};

// A run of rows sharing a heading, in the list's order (Library::sectionCounts): by Title or
// Author the initial ("1A" Latin, "2Ж" Cyrillic, "3#" digits and the rest), by Year the year
// ("" none), by Added the month ("2025-03", "" none).
struct SectionCount {
    std::string key;
    int64_t count = 0;
    bool operator==(const SectionCount&) const = default;
};

// What Title and Author sort by: the initial as SectionCount names it, then 0x1F, then the text
// folded (case, accents). Leading punctuation is skipped; Ё goes with Е, Œ with O.
std::string listSortKey(const std::string& text);

// One line in a list: an item and the file it opens (EPUB preferred over PDF, PDF over a web snapshot).
struct ItemRow {
    std::string key, title, byline, badge /* "EPUB" | "PDF" | "WEB" | "" */, attachmentKey;
    AttachmentRecord::State fileState = AttachmentRecord::State::Remote;
    double progress = -1;  // -1: never opened
    std::string itemType;  // Zotero's: "book", "journalArticle", "preprint", … ("attachment": a lone file)
    bool read = false;     // has the read tag (or was marked read on the Kindle, not sent yet)
    int files = 0;         // its readable files (EPUB, PDF, web snapshot): with more than one, opening asks which
    bool exactFile = false;  // attachmentKey is a file chosen (rowFor with a file): open it without asking
    int64_t lib = kMyLibrary;  // its library (0 My Library, else the group ID)
    bool operator==(const ItemRow&) const = default;
};

// Everything the item details sheet and page show (long-press on an item).
struct ItemDetails {
    ItemRecord item;
    std::string abstract;                      // the abstractNote field, or ""
    std::vector<std::string> collectionPaths;  // "Classics › Novels"
    struct File {
        AttachmentRecord attachment;
        bool onKindle = false;  // downloaded, and the file is there
        int64_t bytes = 0;      // on the Kindle; 0 if not downloaded
        int highlights = 0;     // annotations shown in the reader (not deleted here)
        double progress = -1;   // how far this file was read on the Kindle; -1 if never
    };
    std::vector<File> files;  // readable files first (EPUB, then PDF, then web snapshot), as rows pick them
    double progress = -1;     // how far the item was read; -1 if never opened
    bool read = false;        // as ItemRow::read
};

// One of an item's readable files, as "Open which file?" lists them.
struct FileChoice {
    AttachmentRecord attachment;
    std::string title;      // the attachment's title, else its filename
    bool onKindle = false;  // downloaded
    double progress = -1;   // read on the Kindle (0..1); -1 never opened here
    bool lastRead = false;  // the item's file opened last on this Kindle
};

// Read or unread, set on the Kindle and not sent to Zotero yet: the read tag (as it was when
// marked) to add to the item or take off it.
struct ReadMark {
    std::string itemKey;
    bool read = false;
    std::string tag;
};

// A file's reading position in Zotero's terms (JSON text, as the engines make it: a PDF page
// index, an EPUB point CFI), synced as the setting lastPageIndex_u_<attachmentKey>.
struct ReadingPosition {
    std::string attachmentKey;
    std::string remote;         // what Zotero last said ("" if nothing)
    int64_t remoteVersion = 0;  // its setting version
    std::string local;          // where the Kindle is ("" if never read here)
    bool dirty = false;         // local still to be sent
    bool remoteNew = false;     // Zotero moved elsewhere since the Kindle last agreed: ask on open
    std::string lastSent;       // the Kindle's last upload (its echo is not news, even if its answer was lost)
};

// A bookmark, kept on this Kindle only: the engine's own position of the page start.
struct Bookmark {
    std::string position;  // DocPosition::value
    std::string chapter;   // when it was set (for the list)
    int64_t createdMs = 0;
};

struct CollectionEntry {
    CollectionRecord collection;
    int64_t items = 0;
    bool hasChildren = false;
    bool pinned = false;     // collectionsTab(): pinned to the top
    bool kept = false;       // kept downloaded (keptCollections())
    std::string parentName;  // collectionsTab(): a pinned subcollection's parent
};

struct NoteRow {
    AnnotationRecord annotation;
    std::string itemKey, itemTitle;
};

struct ReadingState {
    std::string itemKey, attachmentKey;
    int64_t openedMs = 0;
    double progress = 0;
    std::string location;
};

// "Kuhn · 1962", "Adda, Cooper · 2003", "Raissi et al. · 2019", "2019", "".
std::string byline(const ItemRecord&);

// A Zotero library on the Kindle: My Library (id 0, always there) or a group (its Zotero group ID).
struct LibraryInfo {
    int64_t id = kMyLibrary;
    std::string name;       // "My Library", or the group's name
    bool enabled = true;    // a group switched off in Settings: not synced, hidden everywhere
    bool gone = false;      // left, deleted, or out of the key's reach: hidden, "No longer available"
    bool canEdit = true;    // the key may write and the group lets this user edit (My Library: true)
    bool canEditFiles = true;
    int64_t items = 0;      // its top-level items (as lists count them)
};

// What removing a group's local copy found (Library::removeLibraryData).
struct LibraryRemoval {
    bool removed = false;
    int unsent = 0;       // Kindle changes not sent yet (annotations, deletions, read marks, plugin changes, positions)
    int conflicts = 0;    // conflict copies kept on the Kindle
    int downloaded = 0;   // files on the Kindle
};

// The key's reach into group libraries (from GET /keys/current; the last discovery's).
enum class GroupAccess { Unknown, None, Some, All };

struct LibraryStats {
    int64_t items = 0, attachments = 0, annotations = 0, collections = 0, conflicts = 0, unsent = 0;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back on destruction unless commit() ran.
class Transaction {
public:
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();
    void commit();

private:
    friend class Library;
    explicit Transaction(Library& lib);
    Library& lib_;
    bool done_ = false;
};

class Library {
public:
    explicit Library(Db&);

    void upsertItem(const ItemRecord&);
    void upsertAttachment(const AttachmentRecord&);
    void upsertAnnotation(const AnnotationRecord&);
    void upsertCollection(const CollectionRecord&);
    // Zotero notes: pulled, never edited or pushed here. notes(itemKey) lists one item's notes
    // ("" lists the standalone ones).
    void upsertNote(const NoteRecord&);
    // note() and notes() hide a note a plugin's pending DeleteNote removes; zoteroNote() doesn't (sync).
    std::optional<NoteRecord> note(const std::string& key);
    std::optional<NoteRecord> zoteroNote(const std::string& key);
    std::vector<NoteRecord> notes(const std::string& itemKey);
    // type: "item", "attachment", "annotation", "note" or "collection". Removing an item or note
    // ends the plugin changes still pending on it (or adding a note to it), and what depends on
    // them, as Refused: "It was deleted in Zotero." — except a pending Trash of the item or
    // DeleteNote of the note by the plugin that added it, which has happened: Sent (another
    // plugin's DeleteNote of it: Refused, "Only a note the plugin added can be deleted.").
    // Downloaded copies go too: an attachment's own, and an item's attachments' (whose rows stay,
    // as Remote, until Zotero deletes them as well).
    void removeObject(const std::string& type, const std::string& key);

    // My Library's sync versions (meta); the `lib` overloads also a group's (its libraries row).
    int64_t libraryVersion();
    void setLibraryVersion(int64_t);
    int64_t libraryVersion(int64_t lib);
    void setLibraryVersion(int64_t lib, int64_t);
    // One-off catch-ups a pull runs ("notes", "attachmentTitles"): per library.
    bool backfillPending(int64_t lib, const std::string& name);
    void clearBackfill(int64_t lib, const std::string& name);

    // ---- Libraries: My Library and the user's groups (spec: group libraries) ----
    // My Library first, then the groups by name. includeHidden: disabled and gone groups too
    // (Settings); otherwise only those shown (lists, filters, plugins).
    std::vector<LibraryInfo> libraries(bool includeHidden = true);
    std::optional<LibraryInfo> libraryInfo(int64_t lib);
    std::string libraryName(int64_t lib);  // "My Library", the group's name, "" if unknown
    // The groups the sync goes through: enabled, not gone, by name.
    std::vector<int64_t> syncedGroups();
    // A group switched on or off (Settings). Off: no longer synced and hidden everywhere; nothing is deleted.
    void setLibraryEnabled(int64_t lib, bool enabled);
    // Whether the Kindle may change things in the library: My Library always; a group when the
    // key can write to it, its editing rules let this user, and it is enabled and not gone.
    bool libraryEditable(int64_t lib);
    // The library holding a key (item, attachment, annotation, note or collection); nullopt if none does.
    std::optional<int64_t> libraryOf(const std::string& key);
    // Whether the Kindle may edit or delete the annotation: in an editable library, and in a group
    // only the user's own (made here, or by the user's Zotero account; authorless ones too).
    bool annotationEditable(const AnnotationRecord&);
    bool annotationEditable(const std::string& key);
    // What removing the group's copy would leave unsent, and how many of its files are on the Kindle.
    LibraryRemoval libraryRemovalCheck(int64_t lib);
    // Removes a group's local copy (items, files on the Kindle, annotations, notes, collections,
    // reading state). Refused (removed false, with the counts) while it has unsent changes or
    // conflicts, unless evenUnsent. A disabled group stays listed, off, to be downloaded again when
    // switched on; a gone one is forgotten. My Library (0) is never removed.
    LibraryRemoval removeLibraryData(int64_t lib, bool evenUnsent = false);
    // Discovery's record of a group (GET /groups/<id>), keeping enabled as the user set it; a new
    // group starts enabled. libraryEditing and fileEditing as Zotero names them ("members", "admins").
    struct GroupMeta {
        int64_t id = 0, version = 0, owner = 0;
        std::string name, libraryEditing, fileEditing;
        bool admin = false;  // the user is the owner or an admin
    };
    void upsertGroup(const GroupMeta&);
    int64_t groupMetaVersion(int64_t lib);  // the version of the group's metadata last recorded (0: none)
    // The key's access to a group, as discovery last read it; recomputes canEdit and canEditFiles.
    void setGroupAccess(int64_t lib, bool read, bool write);
    void setGroupGone(int64_t lib, bool gone);
    GroupAccess keyGroupAccess();  // Unknown before the first discovery
    void setKeyGroupAccess(GroupAccess);
    // The Zotero user this Kindle syncs as (key info): who "mine" is for annotations in groups.
    int64_t userId();
    void setUser(int64_t id, const std::string& name);
    std::string userName();
    // The server version of any object (item, attachment, annotation, note, collection) as last
    // stored; 0 if unknown. Lets an interrupted first sync resume without refetching.
    int64_t objectVersion(const std::string& key);
    void setObjectVersion(const std::string& key, int64_t version);

    // Annotation sync scope (spec §7.4): by default only books opened on the Kindle ("tracked"
    // attachments) have their annotations synced; "Keep all highlights offline" syncs all.
    bool allAnnotationsOffline();
    void setAllAnnotationsOffline(bool);  // turning it off resets annotationsVersion to 0 (every library's)
    int64_t annotationsVersion();         // library version the full annotation sync reached
    void setAnnotationsVersion(int64_t);
    int64_t annotationsVersion(int64_t lib);
    void setAnnotationsVersion(int64_t lib, int64_t);
    void trackAttachment(const std::string& key);
    bool isTracked(const std::string& key);
    std::vector<std::string> trackedAttachments();
    // Removes downloaded annotations of untracked attachments; keeps local edits and conflicts.
    void dropUntrackedAnnotations();

    std::optional<AttachmentRecord> attachment(const std::string& key);
    std::vector<AttachmentRecord> attachmentsOf(const std::string& itemKey);  // every kind, linked too
    std::optional<AnnotationRecord> annotation(const std::string& key);

    std::vector<ItemRecord> recentItems(int limit);  // top-level items, newest dateAdded first
    std::vector<ItemRecord> itemsInCollection(const std::string& key);
    std::vector<CollectionRecord> collections();
    // Every word of `query` must match a word prefix in title, creators, publication or tags (not
    // highlights: recentAnnotations searches those); case and diacritics are ignored.
    std::vector<ItemRecord> search(const std::string& query, int limit);
    std::vector<AnnotationRecord> annotationsFor(const std::string& attachmentKey);
    // Edits and creations; not tombstones. lib: only that library's (kAllLibraries: all).
    std::vector<AnnotationRecord> dirtyAnnotations(int64_t lib = kAllLibraries);

    // Annotation changes made on the Kindle (spec §7.4), pushed by SyncEngine::push.
    // Stores a new annotation under a fresh key (createdHere, dirty, version 0); returns the key.
    std::string createAnnotation(AnnotationRecord);
    // Saves an edit; `version` stays the server version the edit is based on.
    void editAnnotation(const AnnotationRecord&);
    // Never-synced annotations are removed; synced ones become tombstones until the push.
    void deleteAnnotation(const std::string& key);
    // New annotations whose upload has been sent (its answer may have been lost, so Zotero may
    // have them although they are still at version 0): deleting one leaves a tombstone.
    void markUploadSent(const std::vector<std::string>& keys);
    bool uploadSent(const std::string& key);
    std::vector<AnnotationRecord> pendingDeletions(int64_t lib = kAllLibraries);
    std::vector<AnnotationRecord> conflicts();
    void discardConflict(const std::string& key);
    // Puts a conflict copy's content back as a change to send: onto the annotation it was an
    // edit of if that still exists, otherwise as a new annotation. Returns the key it now lives under.
    std::string reapplyConflict(const std::string& key);
    std::vector<AttachmentRecord> readableAttachments(const std::string& itemKey);  // stored EPUB, PDF and web snapshot files only

    LibraryStats stats();

    // Every top-level item's key and raw data JSON, one at a time (identifier indexes: no records built).
    void forEachItemData(const std::function<void(const std::string& key, const std::string& json)>&);
    // SQLite's data_version for this connection: it changes when another connection commits to the
    // database (never for this one's own writes), so a reader can tell the library changed under it.
    int64_t dataVersion();

    // Small app state kept with the library (settings JSON, ...).
    std::string metaValue(const std::string& key);
    void setMetaValue(const std::string& key, const std::string& value);

    // Screens (spec §6): paged lists, counts, collections, notes, reading state, downloads.
    std::vector<ItemRow> rows(const ListQuery&);
    int64_t countRows(const ListQuery&);
    // The sections of rows(q) in order, with how many rows each has (none for Recent). One query.
    std::vector<SectionCount> sectionCounts(const ListQuery&);
    // collectionKey: only items in that collection or its subcollections (any depth).
    std::vector<ItemRow> searchRows(const std::string& query, int offset, int limit, const std::string& collectionKey = "");
    // Searches made on the Kindle, newest first (kRecentSearches at most; kept here only).
    static constexpr size_t kRecentSearches = 8;
    std::vector<std::string> recentSearches();
    void rememberSearch(const std::string& query);  // moves it to the front if it was there
    void clearRecentSearches();
    // The tags put on annotations on this Kindle, most recent first (kRecentAnnotationTags at
    // most; kept here only). The annotation bar's chips are the first four.
    static constexpr size_t kRecentAnnotationTags = 12;
    std::vector<std::string> recentAnnotationTags();
    void rememberAnnotationTag(const std::string& tag);  // moves it to the front if it was there
    // The tags on annotations (not deleted here), most used first, with how many carry each;
    // attachmentKey: only that file's.
    std::vector<std::pair<std::string, int64_t>> annotationTags(const std::string& attachmentKey = "");
    std::optional<ItemRow> rowFor(const std::string& itemKey);
    // The row opening that readable file of the item (exactFile); the item's own row if it has no such file.
    std::optional<ItemRow> rowFor(const std::string& itemKey, const std::string& attachmentKey);
    // The item's readable files: the one opened last first, then EPUB before PDF.
    std::vector<FileChoice> fileChoices(const std::string& itemKey);
    std::optional<ItemDetails> details(const std::string& itemKey);
    // The item as the library shows it: with plugin changes not sent yet applied (tags, collections),
    // and nullopt while a pending plugin change trashes it. zoteroItem: as Zotero has it (as last
    // pulled or sent; a pending-trashed item too) — what the sync engine sends changes against.
    std::optional<ItemRecord> item(const std::string& key);
    std::optional<ItemRecord> zoteroItem(const std::string& key);

    // Read (spec: mark as read): an item is read when it has the read tag. Marking it on the
    // Kindle is kept as a ReadMark until SyncEngine::push has changed the tag in Zotero.
    std::string readTag();                   // "read" unless changed
    void setReadTag(const std::string&);     // "" puts back the default
    bool isRead(const std::string& itemKey);
    void markRead(const std::string& itemKey, bool read);
    std::vector<ReadMark> readMarks(int64_t lib = kAllLibraries);  // lib: of that library's items
    void clearReadMark(const ReadMark&);     // only if it is still this mark (not marked again meanwhile)
    // Deletes the downloaded copy of an attachment (the file and its folder) and marks it as not
    // on the Kindle. Highlights, the reading position and the item itself stay.
    void removeDownload(const std::string& attachmentKey);

    // Reading positions (synced with Zotero's lastPageIndex settings).
    std::optional<ReadingPosition> readingPosition(const std::string& attachmentKey);
    void setLocalPosition(const std::string& attachmentKey, const std::string& value);  // the Kindle moved
    void applyRemotePosition(const std::string& attachmentKey, const std::string& value, int64_t version);  // a pull
    void markPositionSending(const std::string& attachmentKey, const std::string& value);  // before the upload
    void markPositionSent(const std::string& attachmentKey, const std::string& value, int64_t version);
    // The reader has Zotero's position but hasn't compared it with the page it opens on yet:
    // nothing is sent until it answers (or reads on).
    void holdRemotePosition(const std::string& attachmentKey);
    // The reader's Go / Stay about the position at `version` (a newer one pulled meanwhile stays news).
    void answerRemotePosition(const std::string& attachmentKey, bool go, int64_t version);
    std::vector<ReadingPosition> dirtyPositions(int64_t lib = kAllLibraries);  // lib: of that library's files
    int64_t settingsVersion();  // library version the settings pull reached
    // Bookmarks of a file, oldest first (on this Kindle only; Zotero has none).
    std::vector<Bookmark> bookmarks(const std::string& attachmentKey);
    void addBookmark(const std::string& attachmentKey, const Bookmark&);
    void removeBookmark(const std::string& attachmentKey, const std::string& position);
    void setSettingsVersion(int64_t);
    std::vector<std::pair<std::string, int64_t>> topTags(int n);
    // lib: only that library's (with parentKey "": a group's top-level collections).
    std::vector<CollectionEntry> childCollections(const std::string& parentKey /* "" = top level */,
                                                  int64_t lib = kAllLibraries);
    std::vector<CollectionEntry> allCollections();  // every level, by name
    // Pinned collections (a long press): kept on the Kindle, in the order pinned.
    std::vector<std::string> pinnedCollections();
    void setCollectionPinned(const std::string& key, bool pinned);
    // Kept collections (a long press, "Keep downloaded"): the files of their items, their
    // subcollections' too, are downloaded and kept so. On this Kindle only, in the order kept.
    std::vector<std::string> keptCollections();
    void setCollectionKept(const std::string& key, bool kept);  // off deletes nothing
    // What keeping a collection covers: for every item in it or in its subcollections (any depth)
    // that has one, a row per readable file (ItemRow::attachmentKey; the one Open opens first), by title.
    std::vector<ItemRow> collectionFiles(const std::string& collectionKey);
    // The Collections tab: the pinned ones (any level, those still there, of any library shown),
    // then My Library's other top-level ones (a group's are under its own row: childCollections("", id)).
    std::vector<CollectionEntry> collectionsTab();
    // Items whose books were opened on the Kindle, last opened first.
    std::vector<ItemRow> recentlyOpened(int limit);
    // Highlights and notes, newest first; with a query (as search()'s, over their text, comment and
    // tags) only those that match, grouped by item and in book order. tag: only those with that tag.
    std::vector<NoteRow> recentAnnotations(int offset, int limit, const std::string& query = "", const std::string& tag = "");
    int64_t countAnnotations(const std::string& query = "", const std::string& tag = "");
    void markOpened(const std::string& itemKey, const std::string& attachmentKey, int64_t nowMs);
    std::optional<ReadingState> lastOpened();
    std::optional<ReadingState> readingState(const std::string& itemKey);
    // The reader's place in a file (engine position) and how far through it is (0..1): the file's,
    // and the item's (as its last file's). Without a file: the item's last opened (else its row's).
    void saveReading(const std::string& itemKey, const std::string& attachmentKey, const std::string& location,
                     double progress);
    void saveReading(const std::string& itemKey, const std::string& location, double progress);
    std::string readingLocation(const std::string& attachmentKey);  // "" if never read here
    void setFileState(const std::string& attachmentKey, AttachmentRecord::State, const std::string& localPath);
    std::vector<AttachmentRecord> downloadedAttachments();
    std::vector<std::string> attachmentKeys();  // every attachment in the library, whatever its state
    // The readable files of items that have more than one (lib: that library's).
    std::vector<std::string> filesOfManyFileItems(int64_t lib = kAllLibraries);

    // Plugin changes (zk.zotero.*), kept until SyncEngine::push has sent them and applied to the
    // local copy at once: CreateItem inserts the item under a fresh local key, AddNote a note
    // (createdBy = the plugin), Trash and DeleteNote hide the item or note, the others change the
    // item's tags or collections. Items' json stays Zotero's data (the base); their tags and
    // collections are the base with pending changes applied, again after every upsertItem (a pull).
    // Queues one (a state other than Pending only records it in the log, e.g. a Declined removal);
    // returns its id — every change its own, an identical one too (applied as a set, it changes
    // nothing more; sent, it is a no-op). Returns "" and keeps nothing, with the
    // reason in *why, if its item, note, collection or fields aren't there, if its item is being
    // trashed by a pending change (a second trash too), or if a note would go on an attachment.
    std::string queuePluginChange(PluginChange, std::string* why = nullptr);
    std::optional<PluginChange> pluginChange(const std::string& id);
    // Withdraws a Pending change of that plugin, and whatever pending depends on it (of any plugin),
    // putting the local copy back; it becomes Cancelled. False if not pending or not the plugin's.
    bool cancelPluginChange(const std::string& pluginId, const std::string& changeId);
    // Pending changes ("" every plugin's), in creation order with each after what it depends on.
    // lib: only those going to that library (PluginChange::lib).
    std::vector<PluginChange> pendingPluginChanges(const std::string& pluginId = "", int64_t lib = kAllLibraries);
    // A plugin's changes, every state, newest first; finished ones beyond kPluginChangeLog are dropped.
    static constexpr int kPluginChangeLog = 500;
    std::vector<PluginChange> pluginChangeLog(const std::string& pluginId, int limit);
    // Moves a Pending change on (a finished one stays as it is). Sent: its effect becomes part of
    // Zotero's copy here (a trashed item or deleted note goes). Refused, Cancelled, Declined: the
    // local copy is put back, and its pending dependents are withdrawn too (Refused stays Refused).
    void markPluginChange(const std::string& id, PluginChange::State, const std::string& message, int64_t sentMs = 0);
    // Every pending change of the plugin becomes Cancelled, put back; returns how many were withdrawn.
    int discardPluginChanges(const std::string& pluginId);

    Transaction begin();

    // The last storage failure, in Pavel's words (DiskFull when the disk ran out).
    const UserError& lastError() const { return lastError_; }

private:
    friend class Transaction;
    template <class F> auto guarded(F&& f) -> decltype(f());
    void migrate();
    void readSnapshots();  // once: web snapshots became readable (0.7)
    void touch(const std::string& itemKey);  // reindex now, or at commit inside a transaction
    void reindex(const std::string& itemKey);
    std::string itemOfAttachment(const std::string& attachmentKey);
    std::string meta(const std::string& key);
    template <class F> void atomically(F&& f);  // in this transaction, or a new one
    std::string freshKey();
    void overlayPluginChanges(const std::string& itemKey);
    // dependentsMessage: what its pending dependents are told ("" a default per state).
    void withdrawPluginChange(const PluginChange&, PluginChange::State, const std::string& message,
                              const std::string& dependentsMessage = "");
    void prunePluginLog(const std::string& pluginId);
    void setMeta(const std::string& key, const std::string& value);
    int64_t groupColumn(int64_t lib, const char* column);
    void setGroupColumn(int64_t lib, const char* column, int64_t value);
    int unsentIn(int64_t lib);

    Db& db_;
    bool inTx_ = false;
    std::set<std::string> pending_;
    UserError lastError_;
};

}  // namespace zk
