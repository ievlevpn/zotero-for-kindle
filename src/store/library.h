#pragma once
// The local copy of the Zotero library: items, collections, attachments, annotations, and a
// full-text index over item metadata and annotation text. All SQL lives here.

#include "net/errors.h"
#include "store/db.h"
#include "store/records.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zk {

class Library;

// A fresh key in Zotero's alphabet (8 of 23456789ABCDEFGHIJKLMNPQRSTUVWXYZ) for a Kindle-made annotation.
std::string newLocalKey();
bool isZoteroKey(const std::string&);

enum class SortBy { Recent, Title, Author, Year };
// Unread: not read, as ItemRow::read.
enum class Filter { All, Epub, Pdf, Unread };

// The Zotero tag that means "read", unless the user picks another (Settings).
constexpr const char* kDefaultReadTag = "read";

struct ListQuery {
    SortBy sort = SortBy::Recent;
    Filter filter = Filter::All;
    std::string tag, collectionKey;
    int offset = 0, limit = 8;
};

// One line in a list: an item and the file it opens (EPUB preferred over PDF).
struct ItemRow {
    std::string key, title, byline, badge /* "EPUB" | "PDF" | "" */, attachmentKey;
    AttachmentRecord::State fileState = AttachmentRecord::State::Remote;
    double progress = -1;  // -1: never opened
    std::string itemType;  // Zotero's: "book", "journalArticle", "preprint", … ("attachment": a lone file)
    bool read = false;     // has the read tag (or was marked read on the Kindle, not sent yet)
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
    };
    std::vector<File> files;  // readable files first (EPUB, then PDF), as rows pick them
    double progress = -1;     // how far the item was read; -1 if never opened
    bool read = false;        // as ItemRow::read
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
    // type: "item", "attachment", "annotation" or "collection".
    void removeObject(const std::string& type, const std::string& key);

    int64_t libraryVersion();
    void setLibraryVersion(int64_t);
    // The server version of any object (item, attachment, annotation, note, collection) as last
    // stored; 0 if unknown. Lets an interrupted first sync resume without refetching.
    int64_t objectVersion(const std::string& key);
    void setObjectVersion(const std::string& key, int64_t version);

    // Annotation sync scope (spec §7.4): by default only books opened on the Kindle ("tracked"
    // attachments) have their annotations synced; "Keep all highlights offline" syncs all.
    bool allAnnotationsOffline();
    void setAllAnnotationsOffline(bool);  // turning it off resets annotationsVersion to 0
    int64_t annotationsVersion();         // library version the full annotation sync reached
    void setAnnotationsVersion(int64_t);
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
    // Every word of `query` must match a word prefix in title, creators, publication, tags or
    // annotation text/comments; case and diacritics are ignored.
    std::vector<ItemRecord> search(const std::string& query, int limit);
    std::vector<AnnotationRecord> annotationsFor(const std::string& attachmentKey);
    std::vector<AnnotationRecord> dirtyAnnotations();  // edits and creations; not tombstones

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
    std::vector<AnnotationRecord> pendingDeletions();
    std::vector<AnnotationRecord> conflicts();
    void discardConflict(const std::string& key);
    // Puts a conflict copy's content back as a change to send: onto the annotation it was an
    // edit of if that still exists, otherwise as a new annotation. Returns the key it now lives under.
    std::string reapplyConflict(const std::string& key);
    std::vector<AttachmentRecord> readableAttachments(const std::string& itemKey);  // stored EPUB/PDF files only

    LibraryStats stats();

    // Small app state kept with the library (settings JSON, ...).
    std::string metaValue(const std::string& key);
    void setMetaValue(const std::string& key, const std::string& value);

    // Screens (spec §6): paged lists, counts, collections, notes, reading state, downloads.
    std::vector<ItemRow> rows(const ListQuery&);
    int64_t countRows(const ListQuery&);
    // collectionKey: only items in that collection or its subcollections (any depth).
    std::vector<ItemRow> searchRows(const std::string& query, int offset, int limit, const std::string& collectionKey = "");
    // Searches made on the Kindle, newest first (kRecentSearches at most; kept here only).
    static constexpr size_t kRecentSearches = 8;
    std::vector<std::string> recentSearches();
    void rememberSearch(const std::string& query);  // moves it to the front if it was there
    void clearRecentSearches();
    std::optional<ItemRow> rowFor(const std::string& itemKey);
    std::optional<ItemDetails> details(const std::string& itemKey);
    std::optional<ItemRecord> item(const std::string& key);

    // Read (spec: mark as read): an item is read when it has the read tag. Marking it on the
    // Kindle is kept as a ReadMark until SyncEngine::push has changed the tag in Zotero.
    std::string readTag();                   // "read" unless changed
    void setReadTag(const std::string&);     // "" puts back the default
    bool isRead(const std::string& itemKey);
    void markRead(const std::string& itemKey, bool read);
    std::vector<ReadMark> readMarks();
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
    std::vector<ReadingPosition> dirtyPositions();
    int64_t settingsVersion();  // library version the settings pull reached
    // Bookmarks of a file, oldest first (on this Kindle only; Zotero has none).
    std::vector<Bookmark> bookmarks(const std::string& attachmentKey);
    void addBookmark(const std::string& attachmentKey, const Bookmark&);
    void removeBookmark(const std::string& attachmentKey, const std::string& position);
    void setSettingsVersion(int64_t);
    std::vector<std::pair<std::string, int64_t>> topTags(int n);
    std::vector<CollectionEntry> childCollections(const std::string& parentKey /* "" = top level */);
    std::vector<CollectionEntry> allCollections();  // every level, by name
    // Pinned collections (a long press): kept on the Kindle, in the order pinned.
    std::vector<std::string> pinnedCollections();
    void setCollectionPinned(const std::string& key, bool pinned);
    // The Collections tab: the pinned ones (any level, those still there), then the other top-level ones.
    std::vector<CollectionEntry> collectionsTab();
    // Items whose books were opened on the Kindle, last opened first.
    std::vector<ItemRow> recentlyOpened(int limit);
    std::vector<NoteRow> recentAnnotations(int offset, int limit);
    int64_t countAnnotations();
    void markOpened(const std::string& itemKey, const std::string& attachmentKey, int64_t nowMs);
    std::optional<ReadingState> lastOpened();
    std::optional<ReadingState> readingState(const std::string& itemKey);
    // The reader's place in a book (engine position) and how far through it is (0..1).
    void saveReading(const std::string& itemKey, const std::string& location, double progress);
    void setFileState(const std::string& attachmentKey, AttachmentRecord::State, const std::string& localPath);
    std::vector<AttachmentRecord> downloadedAttachments();

    Transaction begin();

    // The last storage failure, in Pavel's words (DiskFull when the disk ran out).
    const UserError& lastError() const { return lastError_; }

private:
    friend class Transaction;
    template <class F> auto guarded(F&& f) -> decltype(f());
    void migrate();
    void touch(const std::string& itemKey);  // reindex now, or at commit inside a transaction
    void reindex(const std::string& itemKey);
    std::string itemOfAttachment(const std::string& attachmentKey);
    std::string meta(const std::string& key);
    void setMeta(const std::string& key, const std::string& value);

    Db& db_;
    bool inTx_ = false;
    std::set<std::string> pending_;
    UserError lastError_;
};

}  // namespace zk
