#include "plugins/events.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>

#include "plugins/identifiers.h"
#include "store/library.h"

namespace zk {

namespace {
class NullSink final : public PluginSink {
public:
    void emit(PluginEvent) override {}
    bool listening() const override { return false; }
};
}  // namespace

PluginSink& nullPluginSink() {
    static NullSink sink;
    return sink;
}

namespace plugins {

using json = nlohmann::ordered_json;

namespace {

json nullable(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

bool downloaded(const AttachmentRecord& a) {
    std::error_code ec;
    return a.state == AttachmentRecord::State::Local && !a.localPath.empty() && std::filesystem::is_regular_file(a.localPath, ec);
}

PluginEvent kindle(const char* name, json payload) { return PluginEvent{name, std::move(payload), true}; }

// Events of at most kMaxListed entries each from `source`; `build` makes an entry's JSON (nullopt:
// leave it out). Each event is built on its own, so the whole list never exists twice.
template <class Source, class Build>
void batched(std::vector<PluginEvent>& out, const char* name, const char* field, const Source& source, Build build) {
    json arr = json::array();
    auto flush = [&] {
        if (arr.empty()) return;
        out.push_back(PluginEvent{name, json{{"origin", "zotero"}, {field, std::move(arr)}}, false});
        arr = json::array();
    };
    for (const auto& x : source) {
        std::optional<json> j = build(x);
        if (!j) continue;
        arr.push_back(std::move(*j));
        if (arr.size() >= kMaxListed) flush();
    }
    flush();
}

}  // namespace

std::string createdOf(const std::string& jsonText, const std::string& fallback) {
    json d = json::parse(jsonText, nullptr, false);
    if (d.is_object()) {
        auto it = d.find("dateAdded");
        if (it != d.end() && it->is_string() && !it->get<std::string>().empty()) return it->get<std::string>();
    }
    return fallback;
}

std::string itemKeyOfAttachment(Library& lib, const std::string& attachmentKey) {
    auto a = lib.attachment(attachmentKey);
    if (!a) return "";
    return a->parentKey.empty() ? a->key : a->parentKey;
}

json annotationJson(const AnnotationRecord& a, const std::string& itemKey) {
    json j = json::object();
    j["key"] = a.key;
    j["attachmentKey"] = a.parentKey;
    j["itemKey"] = itemKey;
    j["type"] = a.type;
    j["text"] = nullable(a.text);
    j["comment"] = nullable(a.comment);
    j["color"] = a.color;
    j["pageLabel"] = nullable(a.pageLabel);
    j["tags"] = a.tags;
    j["created"] = createdOf(a.json, a.dateModified);
    j["modified"] = a.dateModified;
    return j;
}

json attachmentJson(const AttachmentRecord& a) {
    json j = json::object();
    j["key"] = a.key;
    j["itemKey"] = a.parentKey;
    j["title"] = a.filename;  // Zotero's attachment title isn't kept; the file name is the nearest
    j["contentType"] = a.contentType;
    j["filename"] = nullable(a.filename);
    j["downloaded"] = downloaded(a);
    return j;
}

AnnotationRef annotationRef(Library& lib, const AnnotationRecord& a) {
    return {a.key, a.parentKey, itemKeyOfAttachment(lib, a.parentKey)};
}

void annotationEvent(PluginSink& sink, Library& lib, const char* event, const std::string& key) {
    if (!sink.listening()) return;
    auto a = lib.annotation(key);
    if (!a || a->deletedHere || !a->conflictOf.empty()) return;
    sink.emit(kindle(event, json{{"origin", "kindle"},
                                 {"annotations", json::array({annotationJson(*a, itemKeyOfAttachment(lib, a->parentKey))})}}));
}

void annotationDeleted(PluginSink& sink, const AnnotationRef& r) {
    if (!sink.listening()) return;
    sink.emit(kindle("highlightDeleted",
                     json{{"origin", "kindle"},
                          {"annotations", json::array({json{{"key", r.key}, {"attachmentKey", r.attachmentKey}, {"itemKey", r.itemKey}}})}}));
}

void itemMarked(PluginSink& sink, Library& lib, const std::string& itemKey, bool read) {
    if (!sink.listening()) return;
    auto it = lib.item(itemKey);
    if (!it) return;
    sink.emit(kindle(read ? "itemMarkedRead" : "itemMarkedUnread",
                     json{{"origin", "kindle"}, {"items", json::array({itemJson(*it, lib.isRead(itemKey))})}}));
}

void bookEvent(PluginSink& sink, Library& lib, const char* event, const std::string& itemKey, const std::string& attachmentKey) {
    if (!sink.listening()) return;
    auto it = lib.item(itemKey);
    auto att = lib.attachment(attachmentKey);
    if (!it || !att) return;
    sink.emit(kindle(event, json{{"origin", "kindle"}, {"item", itemJson(*it, lib.isRead(itemKey))}, {"attachment", attachmentJson(*att)}}));
}

std::vector<std::string> KeySet::ordered() const {
    std::vector<std::pair<size_t, const std::string*>> by;
    by.reserve(seq_.size());
    for (const auto& [k, n] : seq_) by.emplace_back(n, &k);
    std::sort(by.begin(), by.end());
    std::vector<std::string> out;
    out.reserve(by.size());
    for (const auto& p : by) out.push_back(*p.second);
    return out;
}

void Pulled::annotationCreated(const std::string& key) {
    if (created.has(key)) return;
    changed.erase(key);
    created.add(key);
}
void Pulled::annotationChanged(const std::string& key) {
    if (created.has(key)) return;
    changed.add(key);
}
void Pulled::annotationRemoved(const AnnotationRef& r) {
    const bool newHere = created.has(r.key);
    created.erase(r.key);
    changed.erase(r.key);
    if (!newHere) deleted.push_back(r);
}
void Pulled::itemRead(const std::string& key, bool read) {
    // Marked one way and then back in one pull: no change at all.
    KeySet& same = read ? markedRead : markedUnread;
    KeySet& other = read ? markedUnread : markedRead;
    if (other.has(key)) other.erase(key);
    else same.add(key);
}
bool Pulled::empty() const {
    return created.empty() && changed.empty() && deleted.empty() && markedRead.empty() && markedUnread.empty();
}

std::vector<PluginEvent> pulledEvents(Library& lib, const Pulled& p) {
    std::vector<PluginEvent> out;
    std::map<std::string, std::string> itemOfAttachment;
    auto itemKeyOf = [&](const std::string& attachmentKey) {
        auto it = itemOfAttachment.find(attachmentKey);
        if (it == itemOfAttachment.end()) it = itemOfAttachment.emplace(attachmentKey, itemKeyOfAttachment(lib, attachmentKey)).first;
        return it->second;
    };
    auto annotation = [&](const std::string& key) -> std::optional<json> {
        auto a = lib.annotation(key);
        if (!a || a->deletedHere || !a->conflictOf.empty() || a->conflict) return std::nullopt;
        return annotationJson(*a, itemKeyOf(a->parentKey));
    };
    batched(out, "highlightCreated", "annotations", p.created.ordered(), annotation);
    batched(out, "highlightChanged", "annotations", p.changed.ordered(), annotation);
    batched(out, "highlightDeleted", "annotations", p.deleted, [](const AnnotationRef& r) -> std::optional<json> {
        return json{{"key", r.key}, {"attachmentKey", r.attachmentKey}, {"itemKey", r.itemKey}};
    });
    auto item = [&](const std::string& key) -> std::optional<json> {
        if (auto it = lib.item(key)) return itemJson(*it, lib.isRead(key));
        return std::nullopt;
    };
    batched(out, "itemMarkedRead", "items", p.markedRead.ordered(), item);
    batched(out, "itemMarkedUnread", "items", p.markedUnread.ordered(), item);
    return out;
}

}  // namespace plugins
}  // namespace zk
