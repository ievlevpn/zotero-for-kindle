#pragma once
// Events plugins hear (zk.on, zk.d.ts: Events): where they are sent from and what they carry.
//
// Kindle-side events are sent at once, one per change, with `userCaused` true: through the
// PluginSink the Nav gives every screen (the app forwards them to the plugin host's queue, so the
// UI thread never waits for JavaScript; tests see a recording sink, everything else the null one).
//
// Pulled changes are collected while a pull runs (on the sync worker thread, SyncEngine) and built
// into events when it ends: one event per kind with the list of everything, origin "zotero",
// userCaused false. The first full sync of a library collects nothing, and the app then sends
// only syncFinished (initial true). A list is cut into events of at most kMaxListed entries, so a
// pull of thousands of annotations never makes a giant payload.
//
// Payload shapes: annotations as zk.library.annotations returns them, items as itemJson.

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "store/records.h"

namespace zk {

class Library;

struct PluginEvent {
    std::string name;
    nlohmann::ordered_json payload;  // ordered: an Item's keys come out in zk.d.ts order
    bool userCaused = false;
};

// Where screens (and the app, for what sync brought in) hand events. emit() only queues.
// listening(): false when no plugin would hear an event (none is on): the builders below then
// build nothing, so a Kindle without plugins pays nothing for them.
class PluginSink {
public:
    virtual ~PluginSink() = default;
    virtual void emit(PluginEvent) = 0;
    virtual bool listening() const { return true; }
};
PluginSink& nullPluginSink();  // drops everything; never listening

class FnPluginSink final : public PluginSink {
public:
    explicit FnPluginSink(std::function<void(PluginEvent)> f, std::function<bool()> listening = {})
        : f_(std::move(f)), listening_(std::move(listening)) {}
    void emit(PluginEvent e) override {
        if (f_) f_(std::move(e));
    }
    bool listening() const override { return f_ && (!listening_ || listening_()); }

private:
    std::function<void(PluginEvent)> f_;
    std::function<bool()> listening_;
};

namespace plugins {

// The most entries one batched event lists (annotations or items). More changes make more events.
constexpr size_t kMaxListed = 500;

// zk.d.ts: Annotation, Attachment. The item an annotation belongs to is its attachment's parent
// (a lone attachment is its own item).
nlohmann::ordered_json annotationJson(const AnnotationRecord&, const std::string& itemKey);
nlohmann::ordered_json attachmentJson(const AttachmentRecord&);
std::string itemKeyOfAttachment(Library&, const std::string& attachmentKey);
// "dateAdded" from a record's raw JSON, else `fallback`.
std::string createdOf(const std::string& jsonText, const std::string& fallback);

// What a deleted annotation leaves in its event.
struct AnnotationRef {
    std::string key, attachmentKey, itemKey;
};
AnnotationRef annotationRef(Library&, const AnnotationRecord&);  // call before it is deleted

// ---- On the Kindle (origin "kindle", userCaused true) ----------------------------------------
// "highlightCreated" / "highlightChanged" for the annotation as stored now (nothing if it is gone
// or is a conflict copy, which plugins never see).
void annotationEvent(PluginSink&, Library&, const char* event, const std::string& annotationKey);
void annotationDeleted(PluginSink&, const AnnotationRef&);
// "itemMarkedRead" / "itemMarkedUnread" for an item as it is now.
void itemMarked(PluginSink&, Library&, const std::string& itemKey, bool read);
// "bookOpened" / "bookFinished": the item and the file read.
void bookEvent(PluginSink&, Library&, const char* event, const std::string& itemKey, const std::string& attachmentKey);

// ---- Pulled by a sync (origin "zotero", userCaused false) ------------------------------------
// Keys in the order they were added; add, erase and lookup are O(1) (a pull may bring tens of
// thousands), the order is put together once when the events are built.
class KeySet {
public:
    bool has(const std::string& k) const { return seq_.count(k) != 0; }
    void add(const std::string& k) { seq_.emplace(k, next_++); }  // already there: keeps its place
    void erase(const std::string& k) { seq_.erase(k); }
    bool empty() const { return seq_.empty(); }
    size_t size() const { return seq_.size(); }
    std::vector<std::string> ordered() const;

private:
    std::unordered_map<std::string, size_t> seq_;
    size_t next_ = 0;
};

struct Pulled {
    KeySet created, changed;  // annotation keys
    std::vector<AnnotationRef> deleted;
    KeySet markedRead, markedUnread;  // item keys

    // A key seen twice in one pull counts once: created and then changed is created; created and
    // then deleted never happened; changed and then deleted is deleted.
    void annotationCreated(const std::string& key);
    void annotationChanged(const std::string& key);
    void annotationRemoved(const AnnotationRef&);
    void itemRead(const std::string& key, bool read);
    bool empty() const;
};
// The events for `p`, annotations and items read from `lib` as they are after the pull, in this
// order, each cut into events of at most kMaxListed entries built one after the other: highlightCreated, highlightChanged, highlightDeleted, itemMarkedRead, itemMarkedUnread.
std::vector<PluginEvent> pulledEvents(Library& lib, const Pulled& p);

}  // namespace plugins
}  // namespace zk
