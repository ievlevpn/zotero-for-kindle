#pragma once
// Zotero Web API v3 objects ({"key", "version", "data": {...}}) -> store records. Never throws on
// odd data: missing or mistyped fields read as empty, and the whole `data` object is kept in `json`.

#include "store/records.h"

#include <nlohmann/json.hpp>

namespace zk {

enum class ObjectKind { Item, Attachment, Annotation, Note, Other };

ObjectKind kindOf(const nlohmann::json& obj);
ItemRecord parseItem(const nlohmann::json& obj);
AttachmentRecord parseAttachment(const nlohmann::json& obj);
AnnotationRecord parseAnnotation(const nlohmann::json& obj);
NoteRecord parseNote(const nlohmann::json& obj);
CollectionRecord parseCollection(const nlohmann::json& obj);

}  // namespace zk
