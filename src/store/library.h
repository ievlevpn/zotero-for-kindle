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
    std::optional<AnnotationRecord> annotation(const std::string& key);

    std::vector<ItemRecord> recentItems(int limit);  // top-level items, newest dateAdded first
    std::vector<ItemRecord> itemsInCollection(const std::string& key);
    std::vector<CollectionRecord> collections();
    // Every word of `query` must match a word prefix in title, creators, publication, tags or
    // annotation text/comments; case and diacritics are ignored.
    std::vector<ItemRecord> search(const std::string& query, int limit);
    std::vector<AnnotationRecord> annotationsFor(const std::string& attachmentKey);
    std::vector<AnnotationRecord> dirtyAnnotations();
    std::vector<AttachmentRecord> readableAttachments(const std::string& itemKey);  // stored EPUB/PDF files only

    LibraryStats stats();

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
