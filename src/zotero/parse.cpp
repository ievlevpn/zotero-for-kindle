#include "zotero/parse.h"

namespace zk {

using nlohmann::json;

namespace {

const json& dataOf(const json& obj) {
    static const json empty = json::object();
    if (obj.is_object()) {
        auto it = obj.find("data");
        if (it != obj.end() && it->is_object()) return *it;
    }
    return empty;
}

std::string str(const json& o, const char* name) {
    if (!o.is_object()) return {};
    auto it = o.find(name);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int64_t num(const json& o, const char* name) {
    if (!o.is_object()) return 0;
    auto it = o.find(name);
    if (it == o.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_string()) {  // some fields (mtime) have been seen as strings
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

std::string firstOf(const json& d, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (std::string v = str(d, n); !v.empty()) return v;
    return {};
}

std::string keyOf(const json& obj) {
    std::string k = str(obj, "key");
    return k.empty() ? str(dataOf(obj), "key") : k;
}

int64_t versionOf(const json& obj) {
    int64_t v = num(obj, "version");
    return v ? v : num(dataOf(obj), "version");
}

std::vector<std::string> tagsOf(const json& d) {
    std::vector<std::string> out;
    auto it = d.find("tags");
    if (it == d.end() || !it->is_array()) return out;
    for (const auto& t : *it)
        if (std::string s = str(t, "tag"); !s.empty()) out.push_back(s);
    return out;
}

// The first run of exactly four digits ("2019-02-01" -> "2019", "Spring 1843" -> "1843").
std::string yearOf(const std::string& s) {
    for (size_t i = 0; i + 4 <= s.size(); ++i) {
        auto digit = [&](size_t k) { return k < s.size() && s[k] >= '0' && s[k] <= '9'; };
        if ((i == 0 || !digit(i - 1)) && digit(i) && digit(i + 1) && digit(i + 2) && digit(i + 3) && !digit(i + 4))
            return s.substr(i, 4);
    }
    return {};
}

}  // namespace

ObjectKind kindOf(const json& obj) {
    const std::string type = str(dataOf(obj), "itemType");
    if (type.empty()) return ObjectKind::Other;
    if (type == "attachment") return ObjectKind::Attachment;
    if (type == "annotation") return ObjectKind::Annotation;
    if (type == "note") return ObjectKind::Note;
    return ObjectKind::Item;
}

ItemRecord parseItem(const json& obj) {
    const json& d = dataOf(obj);
    ItemRecord r;
    r.key = keyOf(obj);
    r.version = versionOf(obj);
    r.itemType = str(d, "itemType");
    r.title = firstOf(d, {"title", "caseName", "nameOfAct", "subject"});
    r.date = str(d, "date");
    r.publication = firstOf(d, {"publicationTitle", "bookTitle", "proceedingsTitle", "websiteTitle", "blogTitle",
                                "forumTitle", "encyclopediaTitle", "dictionaryTitle", "programTitle"});
    r.parentKey = str(d, "parentItem");
    if (auto it = d.find("creators"); it != d.end() && it->is_array())
        for (const auto& c : *it)
            if (c.is_object())
                r.creators.push_back({str(c, "firstName"), str(c, "lastName"), str(c, "name"), str(c, "creatorType")});
    if (auto it = d.find("collections"); it != d.end() && it->is_array())
        for (const auto& c : *it)
            if (c.is_string()) r.collections.push_back(c.get<std::string>());
    r.tags = tagsOf(d);
    r.dateAdded = str(d, "dateAdded");
    r.dateModified = str(d, "dateModified");
    r.json = d.dump();
    const json& meta = obj.is_object() && obj.contains("meta") && obj["meta"].is_object() ? obj["meta"] : json::object();
    r.year = yearOf(str(meta, "parsedDate"));
    if (r.year.empty()) r.year = yearOf(r.date);
    return r;
}

AttachmentRecord parseAttachment(const json& obj) {
    const json& d = dataOf(obj);
    AttachmentRecord a;
    a.key = keyOf(obj);
    a.parentKey = str(d, "parentItem");
    a.contentType = str(d, "contentType");
    a.filename = str(d, "filename");
    a.title = str(d, "title");
    a.md5 = str(d, "md5");
    a.linkMode = str(d, "linkMode");
    a.mtime = num(d, "mtime");
    return a;
}

AnnotationRecord parseAnnotation(const json& obj) {
    const json& d = dataOf(obj);
    AnnotationRecord a;
    a.key = keyOf(obj);
    a.version = versionOf(obj);
    a.parentKey = str(d, "parentItem");
    a.type = str(d, "annotationType");
    a.text = str(d, "annotationText");
    a.comment = str(d, "annotationComment");
    a.color = str(d, "annotationColor");
    a.pageLabel = str(d, "annotationPageLabel");
    a.sortIndex = str(d, "annotationSortIndex");
    a.position = str(d, "annotationPosition");
    a.tags = tagsOf(d);
    a.json = d.dump();
    a.dateModified = str(d, "dateModified");
    // In a group: who made it (meta.createdByUser {id, username, name}; name may be empty).
    if (obj.is_object() && obj.contains("meta") && obj["meta"].is_object()) {
        const json& meta = obj["meta"];
        if (auto it = meta.find("createdByUser"); it != meta.end() && it->is_object()) {
            a.authorId = num(*it, "id");
            a.authorName = str(*it, "name");
            if (a.authorName.empty()) a.authorName = str(*it, "username");
        }
    }
    return a;
}

NoteRecord parseNote(const json& obj) {
    const json& d = dataOf(obj);
    NoteRecord n;
    n.key = keyOf(obj);
    n.version = versionOf(obj);
    n.parentKey = str(d, "parentItem");
    n.html = str(d, "note");
    n.json = d.dump();
    n.dateModified = str(d, "dateModified");
    return n;
}

CollectionRecord parseCollection(const json& obj) {
    const json& d = dataOf(obj);
    CollectionRecord c;
    c.key = keyOf(obj);
    c.version = versionOf(obj);
    c.name = str(d, "name");
    c.parentKey = str(d, "parentCollection");  // `false` for top-level collections
    return c;
}

}  // namespace zk
