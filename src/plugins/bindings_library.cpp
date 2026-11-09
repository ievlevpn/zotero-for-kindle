// zk.library: reading the library. The plugin thread opens its own connection to the library
// database at the first call (SQLite wants one per thread, as the sync worker has) and reads
// through Library. Answers are plain JSON (the Item shape is identifiers.h's itemJson).
//
// Pages: items() and recentAnnotations() are async iterables over a native that returns one page
// (50 rows) per call, so a library is walked without being loaded.
//
// findByIdentifier uses a small in-memory index (DOI, arXiv id and ISBN → item keys) built on the
// plugin thread the first time it is needed, from one pass over every item's JSON. It is rebuilt
// when the database changed under us: SQLite's data_version moves whenever another connection
// (the sync worker, the screen thread's writes) commits, and costs nothing to ask. The version is
// read before the pass, so a commit during it only makes the next call rebuild. A rebuild is about
// one JSON parse per item, which a library of several thousand does in well under a second.
//
// fullText is host work: the engine takes seconds on a book, so one worker thread (shared by all
// plugins, started at the first use) extracts while the plugin thread serves others. Jobs run one
// at a time (crengine serialises engines anyway); a job whose call was already stopped is skipped.

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "plugins/fulltext.h"
#include "plugins/identifiers.h"
#include "plugins/plugin_ctx.h"
#include "store/library.h"

namespace zk::plugins {

namespace {

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

constexpr int kSearchLimit = 100;

struct LibState {
    std::unique_ptr<Db> db;
    std::unique_ptr<Library> lib;
    bool indexed = false;
    int64_t dataVersion = 0;
    std::unordered_map<std::string, std::vector<std::string>> byDoi, byArxiv, byIsbn;
};

// The shared state, with a connection; null with a JS error thrown if there is no library.
LibState* stateOf(JSContext* c, PluginCtx& p) {
    auto& slot = librarySlot(p);
    if (!slot) {
        const std::string& path = depsOf(p).dbPath;
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec)) {
            JS_ThrowPlainError(c, "zk.library: the library isn't available");
            return nullptr;
        }
        auto st = std::make_shared<LibState>();
        st->db = std::make_unique<Db>(Db::open(path));
        st->lib = std::make_unique<Library>(*st->db);
        slot = st;
    }
    return static_cast<LibState*>(slot.get());
}

struct Op {
    const char* name;
    Permission perm;
    const char* detail;  // how the permission error names the call
};
const Op kOps[] = {
    {"item", Permission::LibraryRead, "zk.library.item()"},
    {"page", Permission::LibraryRead, "zk.library.items()"},
    {"search", Permission::LibraryRead, "zk.library.search()"},
    {"find", Permission::LibraryRead, "zk.library.findByIdentifier()"},
    {"collections", Permission::LibraryRead, "zk.library.collections()"},
    {"tags", Permission::LibraryRead, "zk.library.tags()"},
    {"attachments", Permission::LibraryRead, "zk.library.attachments()"},
    {"annotations", Permission::AnnotationsRead, "zk.library.annotations()"},
    {"recent", Permission::AnnotationsRead, "zk.library.recentAnnotations()"},
    {"notes", Permission::NotesRead, "zk.library.notes()"},
    {"fullText", Permission::FulltextRead, "zk.library.fullText()"},
};

const Op* opNamed(const std::string& name) {
    for (const Op& o : kOps)
        if (name == o.name) return &o;
    return nullptr;
}

bool text(JSContext* c, JSValueConst v, std::string& out) {
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(c, "zk.library: a string is needed");
        return false;
    }
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) return false;
    out = sanitisedUtf8(s, len);
    JS_FreeCString(c, s);
    return true;
}

bool arg(JSContext* c, int argc, JSValueConst* argv, int i, std::string& out) {
    if (i >= argc) {
        JS_ThrowTypeError(c, "zk.library: an argument is missing");
        return false;
    }
    return text(c, argv[i], out);
}

bool intArg(JSContext* c, int argc, JSValueConst* argv, int i, int& out) {
    int32_t v = 0;
    if (i >= argc || JS_ToInt32(c, &v, argv[i]) < 0) {
        JS_ThrowTypeError(c, "zk.library: a number is needed");
        return false;
    }
    out = v;
    return true;
}

JSValue toJs(JSContext* c, const json& j) {
    const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    return JS_ParseJSON(c, s.data(), s.size(), "<library>");
}

std::string lowerOf(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

json nullable(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

json itemOf(Library& lib, const ItemRecord& r) { return itemJson(r, lib.isRead(r.key)); }

json fieldOf(const std::string& jsonText, const char* key) {
    nlohmann::json d = nlohmann::json::parse(jsonText, nullptr, false);
    if (d.is_object()) {
        auto it = d.find(key);
        if (it != d.end() && it->is_string()) return json(it->get<std::string>());
    }
    return json(nullptr);
}

std::string createdOf(const std::string& jsonText, const std::string& fallback) {
    json added = fieldOf(jsonText, "dateAdded");
    return added.is_string() && !added.get<std::string>().empty() ? added.get<std::string>() : fallback;
}

// The item an attachment belongs to (a lone attachment is its own item).
std::string itemKeyOfAttachment(Library& lib, const std::string& attachmentKey) {
    auto a = lib.attachment(attachmentKey);
    if (!a) return "";
    return a->parentKey.empty() ? a->key : a->parentKey;
}

json annotationOf(const AnnotationRecord& a, const std::string& itemKey) {
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

bool downloaded(const AttachmentRecord& a) {
    std::error_code ec;
    return a.state == AttachmentRecord::State::Local && !a.localPath.empty() && fs::is_regular_file(a.localPath, ec);
}

json attachmentOf(const AttachmentRecord& a) {
    json j = json::object();
    j["key"] = a.key;
    j["itemKey"] = a.parentKey;
    j["title"] = a.filename;  // Zotero's attachment title isn't kept; the file name is the nearest
    j["contentType"] = a.contentType;
    j["filename"] = nullable(a.filename);
    j["downloaded"] = downloaded(a);
    return j;
}

void rebuildIndex(LibState& st) {
    st.dataVersion = st.lib->dataVersion();
    st.byDoi.clear();
    st.byArxiv.clear();
    st.byIsbn.clear();
    st.lib->forEachItemData([&](const std::string& key, const std::string& data) {
        ItemRecord r;
        r.key = key;
        r.json = data;
        const Identifiers id = identifiersOf(r);
        if (!id.doi.empty()) st.byDoi[lowerOf(id.doi)].push_back(key);
        if (!id.arxiv.empty()) st.byArxiv[lowerOf(id.arxiv)].push_back(key);
        for (const std::string& isbn : isbnKeys(id.isbn)) st.byIsbn[isbn].push_back(key);
    });
    st.indexed = true;
}

// ---- Natives ------------------------------------------------------------------------------

// n.need(op): the permission check alone (so an iterable can refuse at the call).
bool need(JSContext* c, PluginCtx& p, const char* opName) {
    const Op* op = opNamed(opName);
    return op && requirePermission(p, op->perm, op->detail);
}

JSValue needNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string name;
    if (!arg(c, argc, argv, 0, name)) return JS_EXCEPTION;
    return need(c, ctxOf(c), name.c_str()) ? JS_UNDEFINED : JS_EXCEPTION;
}

// n.call(op, a, b, c, d): one synchronous read; the arguments are strings or numbers by op.
JSValue callNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string op;
    if (!arg(c, argc, argv, 0, op)) return JS_EXCEPTION;
    if (!need(c, p, op.c_str())) return JS_EXCEPTION;
    LibState* st = stateOf(c, p);
    if (!st) return JS_EXCEPTION;
    Library& lib = *st->lib;
    std::string a, b, d;

    if (op == "item") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        auto r = lib.item(a);
        return toJs(c, r ? itemOf(lib, *r) : json(nullptr));
    }
    if (op == "page") {  // collection, tag, offset, limit
        int offset = 0, limit = 0;
        if (!arg(c, argc, argv, 1, a) || !arg(c, argc, argv, 2, b) || !intArg(c, argc, argv, 3, offset) ||
            !intArg(c, argc, argv, 4, limit))
            return JS_EXCEPTION;
        ListQuery q;
        q.collectionKey = a;
        q.tag = b;
        q.offset = std::max(0, offset);
        q.limit = std::clamp(limit, 1, 200);
        const auto rows = lib.rows(q);
        json items = json::array();
        for (const ItemRow& row : rows)
            if (auto r = lib.item(row.key)) items.push_back(itemOf(lib, *r));
        return toJs(c, json{{"rows", rows.size()}, {"items", items}});
    }
    if (op == "search") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        json items = json::array();
        for (const ItemRecord& r : lib.search(a, kSearchLimit)) items.push_back(itemOf(lib, r));
        return toJs(c, items);
    }
    if (op == "find") {  // doi, arxiv, isbn ("" for none)
        if (!arg(c, argc, argv, 1, a) || !arg(c, argc, argv, 2, b) || !arg(c, argc, argv, 3, d)) return JS_EXCEPTION;
        if (!st->indexed || lib.dataVersion() != st->dataVersion) rebuildIndex(*st);
        std::vector<std::string> keys;
        auto add = [&](const std::unordered_map<std::string, std::vector<std::string>>& index, const std::string& k) {
            auto it = index.find(k);
            if (k.empty() || it == index.end()) return;
            for (const std::string& key : it->second)
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
        };
        add(st->byDoi, doiKey(a));
        add(st->byArxiv, lowerOf(normaliseArxiv(b)));
        for (const std::string& isbn : isbnKeys(d)) add(st->byIsbn, isbn);
        json items = json::array();
        for (const std::string& key : keys)
            if (auto r = lib.item(key)) items.push_back(itemOf(lib, *r));
        return toJs(c, items);
    }
    if (op == "collections") {
        std::map<std::string, CollectionRecord> byKey;
        const auto all = lib.allCollections();
        for (const CollectionEntry& e : all) byKey[e.collection.key] = e.collection;
        struct Row {
            std::vector<std::string> path, sortPath;
            CollectionRecord c;
        };
        std::vector<Row> rows;
        for (const CollectionEntry& e : all) {
            Row row{{}, {}, e.collection};
            std::set<std::string> seen;
            for (const CollectionRecord* cur = &e.collection; cur;) {
                if (!seen.insert(cur->key).second) break;  // a loop in the data: stop
                row.path.insert(row.path.begin(), cur->name);
                auto up = byKey.find(cur->parentKey);
                cur = up == byKey.end() ? nullptr : &up->second;
            }
            for (const std::string& n : row.path) row.sortPath.push_back(lowerOf(n));
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
            return std::tie(x.sortPath, x.path, x.c.key) < std::tie(y.sortPath, y.path, y.c.key);
        });
        json out = json::array();
        for (const Row& row : rows)
            out.push_back({{"key", row.c.key}, {"name", row.c.name}, {"parent", nullable(row.c.parentKey)}, {"path", row.path}});
        return toJs(c, out);
    }
    if (op == "tags") {
        auto tags = lib.topTags(1 << 30);
        std::sort(tags.begin(), tags.end(), [](const auto& x, const auto& y) {
            const std::string lx = lowerOf(x.first), ly = lowerOf(y.first);
            return lx != ly ? lx < ly : x.first < y.first;
        });
        json out = json::array();
        for (const auto& [name, count] : tags) out.push_back({{"name", name}, {"count", count}});
        return toJs(c, out);
    }
    if (op == "attachments") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        json out = json::array();
        for (const AttachmentRecord& at : lib.attachmentsOf(a)) out.push_back(attachmentOf(at));
        return toJs(c, out);
    }
    if (op == "annotations") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        const std::string itemKey = itemKeyOfAttachment(lib, a);
        json out = json::array();
        for (const AnnotationRecord& an : lib.annotationsFor(a))
            if (an.conflictOf.empty()) out.push_back(annotationOf(an, itemKey));
        return toJs(c, out);
    }
    if (op == "recent") {  // offset, limit
        int offset = 0, limit = 0;
        if (!intArg(c, argc, argv, 1, offset) || !intArg(c, argc, argv, 2, limit)) return JS_EXCEPTION;
        const auto rows = lib.recentAnnotations(std::max(0, offset), std::clamp(limit, 1, 200));
        json items = json::array();
        for (const NoteRow& row : rows)
            if (row.annotation.conflictOf.empty()) items.push_back(annotationOf(row.annotation, row.itemKey));
        return toJs(c, json{{"rows", rows.size()}, {"items", items}});
    }
    if (op == "notes") {
        if (!arg(c, argc, argv, 1, a)) return JS_EXCEPTION;
        auto notes = lib.notes(a);
        std::vector<std::pair<std::string, const NoteRecord*>> order;  // oldest first
        for (const NoteRecord& n : notes) order.emplace_back(createdOf(n.json, n.dateModified), &n);
        std::stable_sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        json out = json::array();
        for (const auto& [created, n] : order)
            out.push_back({{"key", n->key}, {"itemKey", n->parentKey}, {"html", n->html}, {"createdBy", nullable(n->createdBy)},
                           {"created", created}, {"modified", n->dateModified}});
        return toJs(c, out);
    }
    return JS_ThrowTypeError(c, "zk.library: unknown call");
}

// ---- Full text: one worker for every plugin ------------------------------------------------

struct FullTextJob {
    AttachmentRecord attachment;
    std::string pluginsDir;
    EngineEnv env;
    std::shared_ptr<Work> work;
};

// Started at the first use and never stopped: the thread sleeps between jobs, and a job in flight
// at exit is the process's own business. Everything a job needs is copied into it, so no plugin
// or host object is touched from here (Work::finish is safe from any thread, and a no-op once the
// host is gone).
class FullTextWorker {
public:
    static FullTextWorker& get() {
        static FullTextWorker* w = new FullTextWorker;
        return *w;
    }
    void submit(FullTextJob job) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

private:
    FullTextWorker() { std::thread([this] { run(); }).detach(); }
    void run() {
        for (;;) {
            FullTextJob job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return !jobs_.empty(); });
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            if (job.work->cancelled()) continue;  // its call was stopped while it waited
            UserError err;
            auto out = extractFullText(job.attachment, job.pluginsDir, job.env, &err);
            if (out) {
                auto textPtr = std::make_shared<std::string>(std::move(*out));
                job.work->finish([textPtr](JSContext* c) {
                    const std::string clean = sanitisedUtf8(textPtr->data(), textPtr->size());
                    return JS_NewStringLen(c, clean.data(), clean.size());
                });
            } else {
                job.work->reject(err.message.empty() ? "zk.library.fullText: the text can't be read" : err.message);
            }
        }
    }
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<FullTextJob> jobs_;
};

JSValue fullTextNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    if (!need(c, p, "fullText")) return JS_EXCEPTION;
    std::string key;
    if (!arg(c, argc, argv, 0, key)) return JS_EXCEPTION;
    LibState* st = stateOf(c, p);
    if (!st) return JS_EXCEPTION;
    auto attachment = st->lib->attachment(key);
    JSValue promise;
    auto work = startWork(p, &promise);
    if (JS_IsException(promise) || work->cancelled()) return promise;
    if (!attachment) {
        work->reject("There is no attachment " + key + " in the library.");
        return promise;
    }
    const PluginHostDeps& deps = depsOf(p);
    FullTextWorker::get().submit({std::move(*attachment), deps.pluginsDir, deps.engineEnv, work});
    return promise;
}

// ---- What plugins see ---------------------------------------------------------------------

const char kLibraryJs[] = R"JS((function (n) {
"use strict";
const PAGE = 50;

function key(v, what) {
  if (typeof v !== "string") throw new TypeError(`zk.library.${what}: the key must be a string`);
  return v;
}

function limitOf(v, what) {
  if (v === undefined) return Infinity;
  if (!Number.isInteger(v) || v < 1) throw new TypeError(`zk.library.${what}: limit must be a positive whole number`);
  return v;
}

// An async iterable over pages: fetchPage(offset, size) -> {rows, items}.
function paged(fetchPage, limit, stopAt) {
  return {
    async *[Symbol.asyncIterator]() {
      let offset = 0, given = 0;
      for (;;) {
        const size = Math.min(PAGE, limit - given);
        if (size <= 0) return;
        const page = fetchPage(offset, size);
        for (const item of page.items) {
          if (stopAt && stopAt(item)) return;
          yield item;
          if (++given >= limit) return;
        }
        if (page.rows < size) return;
        offset += page.rows;
      }
    },
  };
}

const library = {
  items(query) {
    n.need("page");
    if (query !== undefined && (query === null || typeof query !== "object")) throw new TypeError("zk.library.items: the query must be an object");
    const q = query || {};
    for (const f of ["collection", "tag"])
      if (q[f] !== undefined && typeof q[f] !== "string") throw new TypeError(`zk.library.items: ${f} must be a string`);
    const limit = limitOf(q.limit, "items");
    const collection = q.collection === undefined ? "" : q.collection;
    const tag = q.tag === undefined ? "" : q.tag;
    return paged((offset, size) => n.call("page", collection, tag, offset, size), limit);
  },
  async item(k) { n.need("item"); return n.call("item", key(k, "item")); },
  async findByIdentifier(id) {
    n.need("find");
    if (id === null || typeof id !== "object") throw new TypeError("zk.library.findByIdentifier: pass { doi, arxiv, isbn }");
    for (const f of ["doi", "arxiv", "isbn"])
      if (id[f] !== undefined && typeof id[f] !== "string") throw new TypeError(`zk.library.findByIdentifier: ${f} must be a string`);
    return n.call("find", id.doi || "", id.arxiv || "", id.isbn || "");
  },
  async collections() { n.need("collections"); return n.call("collections"); },
  async tags() { n.need("tags"); return n.call("tags"); },
  async search(text) {
    n.need("search");
    if (typeof text !== "string") throw new TypeError("zk.library.search: the text must be a string");
    return n.call("search", text);
  },
  async attachments(itemKey) { n.need("attachments"); return n.call("attachments", key(itemKey, "attachments")); },
  async annotations(attachmentKey) { n.need("annotations"); return n.call("annotations", key(attachmentKey, "annotations")); },
  recentAnnotations(query) {
    n.need("recent");
    if (query !== undefined && (query === null || typeof query !== "object")) throw new TypeError("zk.library.recentAnnotations: the query must be an object");
    const q = query || {};
    const limit = limitOf(q.limit, "recentAnnotations");
    let since = null;
    if (q.since !== undefined) {
      since = typeof q.since === "string" ? Date.parse(q.since) : NaN;
      if (Number.isNaN(since)) throw new TypeError("zk.library.recentAnnotations: since must be an ISO date string");
    }
    return paged((offset, size) => n.call("recent", offset, size), limit,
                 since === null ? undefined : (a) => Date.parse(a.modified) < since);
  },
  async notes(itemKey) { n.need("notes"); return n.call("notes", key(itemKey, "notes")); },
  async fullText(attachmentKey) { n.need("fullText"); return n.fullText(key(attachmentKey, "fullText")); },
};
return library;
}))JS";

}  // namespace

void installLibrary(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction<needNative>(c, natives.raw(), "need", 1);
    defineFunction<callNative>(c, natives.raw(), "call", 5);
    defineFunction<fullTextNative>(c, natives.raw(), "fullText", 1);

    const char* name = "<zk library>";
    js::Value fn(c, JS_Eval(c, kLibraryJs, sizeof kLibraryJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) {
        js::Value ex(c, JS_GetException(c));
        js::Value msg = ex.get("message");
        size_t len = 0;
        const char* s = JS_ToCStringLen(c, &len, msg.raw());
        std::string t = s ? std::string(s, len) : "?";
        if (s) JS_FreeCString(c, s);
        throw std::runtime_error(std::string(name) + ": " + t);
    }
    js::Value library = p.cx.call(fn, {natives});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
    JS_SetPropertyStr(c, p.zk.raw(), "library", JS_DupValue(c, library.raw()));
}

}  // namespace zk::plugins
