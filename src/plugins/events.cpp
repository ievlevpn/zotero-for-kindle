#include "plugins/events.h"

#include <algorithm>
#include <filesystem>
#include <map>

#include "plugins/identifiers.h"
#include "store/library.h"

namespace zk {

namespace {
class NullSink final : public PluginSink {
public:
    void emit(PluginEvent) override {}
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

template <class T>
void drop(std::vector<T>& v, const T& x) {
    v.erase(std::remove(v.begin(), v.end(), x), v.end());
}
bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// Emits `list` as events of at most kMaxListed entries each.
void batched(std::vector<PluginEvent>& out, const char* name, const char* field, const std::vector<json>& list) {
    for (size_t i = 0; i < list.size(); i += kMaxListed) {
        json arr = json::array();
        for (size_t j = i; j < std::min(list.size(), i + kMaxListed); ++j) arr.push_back(list[j]);
        out.push_back(PluginEvent{name, json{{"origin", "zotero"}, {field, std::move(arr)}}, false});
    }
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
    auto a = lib.annotation(key);
    if (!a || a->deletedHere || !a->conflictOf.empty()) return;
    sink.emit(kindle(event, json{{"origin", "kindle"},
                                 {"annotations", json::array({annotationJson(*a, itemKeyOfAttachment(lib, a->parentKey))})}}));
}

void annotationDeleted(PluginSink& sink, const AnnotationRef& r) {
    sink.emit(kindle("highlightDeleted",
                     json{{"origin", "kindle"},
                          {"annotations", json::array({json{{"key", r.key}, {"attachmentKey", r.attachmentKey}, {"itemKey", r.itemKey}}})}}));
}

void itemMarked(PluginSink& sink, Library& lib, const std::string& itemKey, bool read) {
    auto it = lib.item(itemKey);
    if (!it) return;
    sink.emit(kindle(read ? "itemMarkedRead" : "itemMarkedUnread",
                     json{{"origin", "kindle"}, {"items", json::array({itemJson(*it, lib.isRead(itemKey))})}}));
}

void bookEvent(PluginSink& sink, Library& lib, const char* event, const std::string& itemKey, const std::string& attachmentKey) {
    auto it = lib.item(itemKey);
    auto att = lib.attachment(attachmentKey);
    if (!it || !att) return;
    sink.emit(kindle(event, json{{"origin", "kindle"}, {"item", itemJson(*it, lib.isRead(itemKey))}, {"attachment", attachmentJson(*att)}}));
}

void Pulled::annotationCreated(const std::string& key) {
    if (has(created, key)) return;
    drop(changed, key);
    created.push_back(key);
}
void Pulled::annotationChanged(const std::string& key) {
    if (has(created, key) || has(changed, key)) return;
    changed.push_back(key);
}
void Pulled::annotationRemoved(const AnnotationRef& r) {
    const bool newHere = has(created, r.key);
    drop(created, r.key);
    drop(changed, r.key);
    if (!newHere) deleted.push_back(r);
}
void Pulled::itemRead(const std::string& key, bool read) {
    // Marked one way and then back in one pull: no change at all.
    auto& same = read ? markedRead : markedUnread;
    auto& other = read ? markedUnread : markedRead;
    if (has(other, key)) drop(other, key);
    else if (!has(same, key)) same.push_back(key);
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
    auto annotations = [&](const std::vector<std::string>& keys) {
        std::vector<json> list;
        for (const std::string& key : keys) {
            auto a = lib.annotation(key);
            if (!a || a->deletedHere || !a->conflictOf.empty() || a->conflict) continue;
            list.push_back(annotationJson(*a, itemKeyOf(a->parentKey)));
        }
        return list;
    };
    batched(out, "highlightCreated", "annotations", annotations(p.created));
    batched(out, "highlightChanged", "annotations", annotations(p.changed));
    std::vector<json> gone;
    for (const AnnotationRef& r : p.deleted)
        gone.push_back(json{{"key", r.key}, {"attachmentKey", r.attachmentKey}, {"itemKey", r.itemKey}});
    batched(out, "highlightDeleted", "annotations", gone);
    auto items = [&](const std::vector<std::string>& keys) {
        std::vector<json> list;
        for (const std::string& key : keys)
            if (auto it = lib.item(key)) list.push_back(itemJson(*it, lib.isRead(key)));
        return list;
    };
    batched(out, "itemMarkedRead", "items", items(p.markedRead));
    batched(out, "itemMarkedUnread", "items", items(p.markedUnread));
    return out;
}

}  // namespace plugins
}  // namespace zk
