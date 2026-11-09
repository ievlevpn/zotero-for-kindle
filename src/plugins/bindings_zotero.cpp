// zk.zotero: changes a plugin asks of the Zotero library. Plugins never write to Zotero: each change
// goes into the library's pending plugin changes (Library::queuePluginChange), which applies it to
// the local copy at once and keeps it until the sync engine has sent it. The app is told
// (ChangesQueued) so it pushes soon.
//
// Adds (createItem, addToCollection, addTag, addNote) are granted by permission and resolve at once.
// Removals (trash, removeFromCollection, removeTag, deleteNote) need the user's yes each time: the
// app shows its own confirmation (a Confirm event; PluginHost::answer). Only a call the user caused
// may ask; from a background task, a sync's event or main.js they resolve null without asking.
// Removals asked in one go — before the plugin's code awaits any of them, e.g. Promise.all — are
// one question: the first one queues a promise job that waits until every other pending job of
// the plugin's code has run (re-queuing itself behind them), then asks for all of them at once.
// A yes queues each change and resolves its Change; a no records each as Declined (the change log)
// and resolves null. A stopped call's question is withdrawn by the host and its promises reject
// with LimitError (the removals are Works of the call).

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <stdexcept>
#include <vector>

#include "plugins/plugin_ctx.h"
#include "store/library.h"

namespace zk::plugins {

namespace {

using json = nlohmann::ordered_json;
using Kind = PluginChange::Kind;

// ---- Wording ------------------------------------------------------------------------------

// `s` cut to at most `max` characters (code points), at a word if one ends near, with "…".
std::string shortened(const std::string& s, size_t max) {
    size_t chars = 0, cut = std::string::npos;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;  // inside a character
        if (chars == max) {
            cut = i;
            break;
        }
        ++chars;
    }
    if (cut == std::string::npos) return s;
    std::string out = s.substr(0, cut);
    const size_t space = out.rfind(' ');
    if (space != std::string::npos && space + 16 >= out.size()) out.resize(space);  // a whole word
    while (!out.empty() && (out.back() == ' ' || out.back() == ',' || out.back() == ';' || out.back() == ':' || out.back() == '-'))
        out.pop_back();
    return out + "…";
}

// Text from the library or the plugin as the confirmation shows it: one line, shortened.
std::string shown(const std::string& s, size_t max) { return shortened(oneLine(s), max); }
std::string quotedTitle(const std::string& title) {
    const std::string t = oneLine(title);
    return "'" + shortened(t.empty() ? "Untitled" : t, 60) + "'";
}

// One removal as the confirmation names it.
struct Removal {
    std::shared_ptr<Work> work;
    PluginChange change;
    std::string phrase;  // after "<Plugin> wants to ": "move 'X' to the Zotero trash"
    std::string title;   // the item's title, quoted
};

std::string actionOf(Kind k) { return k == Kind::Trash ? "Move to trash" : k == Kind::DeleteNote ? "Delete" : "Remove"; }

std::string capitalised(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// The question for removals asked together.
PluginHostEvent questionFor(const std::string& pluginName, const std::vector<Removal>& asked) {
    PluginHostEvent ev{};
    ev.kind = PluginHostEvent::Kind::Confirm;
    const Kind first = asked.front().change.kind;
    bool same = true;
    for (const Removal& r : asked) same = same && r.change.kind == first;
    const std::string name = oneLine(pluginName), n = std::to_string(asked.size());
    if (asked.size() == 1) {
        ev.text = name + " wants to " + asked.front().phrase + ".";
    } else if (same && first == Kind::Trash) {  // the total in the question: the list may not all fit
        ev.text = name + " wants to move these " + n + " items to the Zotero trash:";
        for (const Removal& r : asked) ev.lines.push_back(r.title);
    } else {
        ev.text = name + " wants to make these " + n + " changes:";
        for (const Removal& r : asked) ev.lines.push_back(capitalised(r.phrase));
    }
    ev.action = same ? actionOf(first) : "Allow";
    return ev;
}

// ---- State --------------------------------------------------------------------------------

struct ZoteroState {
    std::vector<Removal> asked;  // removals waiting to be asked together
    bool flushQueued = false;
};

ZoteroState& stateOf(PluginCtx& p) {
    if (!p.zotero) p.zotero = std::make_shared<ZoteroState>();
    return *static_cast<ZoteroState*>(p.zotero.get());
}

Library& libraryOf(PluginCtx& p) {
    std::string why;
    Library* lib = sharedLibrary(p, &why);
    if (!lib) throw std::runtime_error("zk.zotero: " + why);  // an Error("The app failed doing this: …")
    return *lib;
}

void changesQueued(PluginCtx& p) {
    PluginHostEvent ev{};
    ev.kind = PluginHostEvent::Kind::ChangesQueued;
    post(p, std::move(ev));
}

// ---- Helpers ------------------------------------------------------------------------------

bool text(JSContext* c, int argc, JSValueConst* argv, int i, std::string& out) {
    if (i >= argc || !JS_IsString(argv[i])) {
        JS_ThrowTypeError(c, "zk.zotero: a string is needed");
        return false;
    }
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, argv[i]);
    if (!s) return false;
    out = sanitisedUtf8(s, len);
    JS_FreeCString(c, s);
    return true;
}

JSValue toJs(JSContext* c, const json& j) {
    const std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    return JS_ParseJSON(c, s.data(), s.size(), "<zotero>");
}

bool kindNamed(const std::string& name, Kind& out) { return kindFromName(name, out); }

std::string isoOf(int64_t ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

// ---- Natives ------------------------------------------------------------------------------

// n.need(call, withCollections, withTags): the permission check alone.
JSValue needNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string name;
    if (!text(c, argc, argv, 0, name)) return JS_EXCEPTION;
    Kind k;
    if (!kindNamed(name, k)) return JS_ThrowTypeError(c, "zk.zotero: unknown call");
    const std::string detail = "zk.zotero." + name + "()";
    switch (k) {
        case Kind::CreateItem: {
            if (!requirePermission(p, Permission::CreateItems, detail)) return JS_EXCEPTION;
            if (argc > 1 && JS_ToBool(c, argv[1]) && !requirePermission(p, Permission::Collections, detail + " with collections"))
                return JS_EXCEPTION;
            if (argc > 2 && JS_ToBool(c, argv[2]) && !requirePermission(p, Permission::Tags, detail + " with tags")) return JS_EXCEPTION;
            break;
        }
        case Kind::AddToCollection:
            if (!requirePermission(p, Permission::Collections, detail)) return JS_EXCEPTION;
            break;
        case Kind::AddTag:
            if (!requirePermission(p, Permission::Tags, detail)) return JS_EXCEPTION;
            break;
        case Kind::AddNote:
            if (!requirePermission(p, Permission::Notes, detail)) return JS_EXCEPTION;
            break;
        default:
            if (!requirePermission(p, Permission::Remove, detail)) return JS_EXCEPTION;
    }
    return JS_UNDEFINED;
}

// n.add(call, a, b): createItem(valueJson), addToCollection(item, collection), addTag(item, tag),
// addNote(item, html). Returns the Change, or throws an Error with the library's reason.
JSValue addNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string name, a, b;
    if (!text(c, argc, argv, 0, name) || !text(c, argc, argv, 1, a)) return JS_EXCEPTION;
    Kind k;
    if (!kindNamed(name, k) || (k != Kind::CreateItem && k != Kind::AddToCollection && k != Kind::AddTag && k != Kind::AddNote))
        return JS_ThrowTypeError(c, "zk.zotero: unknown call");
    if (k != Kind::CreateItem && !text(c, argc, argv, 2, b)) return JS_EXCEPTION;
    PluginChange ch;
    ch.pluginId = p.manifest.id;
    ch.kind = k;
    if (k == Kind::CreateItem) ch.value = a;
    else if (k == Kind::AddNote) {
        ch.parentKey = a;
        ch.value = b;
    } else {
        ch.itemKey = a;
        ch.value = b;
    }
    Library& lib = libraryOf(p);
    std::string why;
    const std::string id = lib.queuePluginChange(ch, &why);
    if (id.empty()) return JS_ThrowPlainError(c, "zk.zotero.%s: %s", name.c_str(), why.empty() ? "refused" : why.c_str());
    auto made = lib.pluginChange(id);
    changesQueued(p);
    return toJs(c, json{{"key", made ? made->itemKey : ch.itemKey}, {"changeId", id}});
}

JSValue flushJob(JSContext* c, int, JSValueConst*);

// n.remove(call, key, value): null if this call may not ask (nothing asked), else a promise for
// the Change (yes) or null (no), asked together with the other removals of this turn.
JSValue removeNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string name, key, value;
    if (!text(c, argc, argv, 0, name) || !text(c, argc, argv, 1, key) || !text(c, argc, argv, 2, value)) return JS_EXCEPTION;
    Kind k;
    if (!kindNamed(name, k) ||
        (k != Kind::Trash && k != Kind::RemoveFromCollection && k != Kind::RemoveTag && k != Kind::DeleteNote))
        return JS_ThrowTypeError(c, "zk.zotero: unknown call");
    Library& lib = libraryOf(p);

    Removal r;
    r.change.pluginId = p.manifest.id;
    r.change.kind = k;
    r.change.itemKey = key;
    if (k == Kind::RemoveFromCollection || k == Kind::RemoveTag) r.change.value = value;
    // A removal that would change nothing (already gone, or not there to remove) resolves null
    // without asking: nothing to confirm.
    auto nothing = [&](const std::string& why) {
        log(p, LogLevel::Info, "zk.zotero." + name + "(" + key + (value.empty() ? "" : ", " + value) + "): " + why +
                                   " — not asked, resolved null");
        return JS_NULL;
    };
    if (k == Kind::DeleteNote) {
        auto note = lib.note(key);
        if (!note) {
            if (lib.zoteroNote(key)) return nothing("the note is being deleted already");
            return JS_ThrowPlainError(c, "zk.zotero.deleteNote: no note %s", key.c_str());
        }
        if (note->createdBy != p.manifest.id)
            return JS_ThrowPlainError(c, "zk.zotero.deleteNote: only a note this plugin added can be deleted (%s isn't)", key.c_str());
        auto parent = lib.zoteroItem(note->parentKey);
        r.title = quotedTitle(parent ? parent->title : "");
        r.phrase = "delete a note it added to " + r.title;
    } else {
        auto item = lib.item(key);
        if (!item) {
            if (lib.zoteroItem(key)) return nothing("the item is being moved to the trash already");
            return JS_ThrowPlainError(c, "zk.zotero.%s: no item %s", name.c_str(), key.c_str());
        }
        r.title = quotedTitle(item->title);
        if (k == Kind::Trash) {
            r.phrase = "move " + r.title + " to the Zotero trash";
        } else if (k == Kind::RemoveTag) {
            if (std::find(item->tags.begin(), item->tags.end(), value) == item->tags.end())
                return nothing("the item doesn't have that tag");
            r.phrase = "remove the tag '" + shown(value, 40) + "' from " + r.title;
        } else {
            if (std::find(item->collections.begin(), item->collections.end(), value) == item->collections.end())
                return nothing("the item isn't in that collection");
            std::string collection = value;
            for (const CollectionRecord& col : lib.collections())
                if (col.key == value) collection = col.name;
            r.phrase = "remove " + r.title + " from " + shown(collection, 40);
        }
    }

    const CallInfo call = currentCall(p);
    if (!call.userCaused || call.background || call.cleanup) {
        log(p, LogLevel::Info,
            "zk.zotero." + name + "(" + key + "): not asked, resolved null — removals are only asked in answer to something the "
            "user did, not from " + (call.background ? "a background task" : "code the user didn't cause (a sync's event, a timer, loading)"));
        return JS_NULL;
    }
    JSValue promise;
    r.work = startWork(p, &promise);
    if (JS_IsException(promise) || r.work->cancelled()) return promise;
    ZoteroState& st = stateOf(p);
    st.asked.push_back(std::move(r));
    if (!st.flushQueued) {
        if (JS_EnqueueJob(c, flushJob, 0, nullptr) < 0) return JS_EXCEPTION;
        st.flushQueued = true;
    }
    return promise;
}

// Settles removals the user answered (plugin thread, outside JS).
void answered(PluginCtx& p, const std::vector<Removal>& asked, bool yes) {
    bool queued = false;
    for (const Removal& r : asked) {
        if (r.work->cancelled()) continue;
        try {
            Library& lib = libraryOf(p);
            PluginChange ch = r.change;
            if (!yes) {
                ch.state = PluginChange::State::Declined;
                lib.queuePluginChange(ch);  // the change log: declined by the user
                r.work->resolve(nullptr);
                continue;
            }
            std::string why;
            const std::string id = lib.queuePluginChange(ch, &why);
            if (id.empty()) {
                r.work->reject(std::string("zk.zotero.") + kindName(ch.kind) + ": " + (why.empty() ? "refused" : why));
                continue;
            }
            r.work->resolve(nlohmann::json{{"key", ch.itemKey}, {"changeId", id}});
            queued = true;
        } catch (const std::exception& e) {
            r.work->reject(std::string("The app failed doing this: ") + e.what());
        }
    }
    if (queued) changesQueued(p);
}

// Asks for the removals of this turn, once every other pending job has run.
JSValue flushJob(JSContext* c, int, JSValueConst*) {
    try {
        PluginCtx& p = ctxOf(c);
        if (JS_IsJobPending(JS_GetRuntime(c))) {  // more of the plugin's code to run first: after it
            if (JS_EnqueueJob(c, flushJob, 0, nullptr) == 0) return JS_UNDEFINED;
        }
        ZoteroState& st = stateOf(p);
        st.flushQueued = false;
        std::vector<Removal> asked;
        for (Removal& r : st.asked)
            if (!r.work->cancelled()) asked.push_back(std::move(r));
        st.asked.clear();
        if (asked.empty()) return JS_UNDEFINED;
        PluginHostEvent question = questionFor(p.manifest.name, asked);
        auto shared = std::make_shared<std::vector<Removal>>(std::move(asked));
        PluginCtx* ctx = &p;  // the callback is dropped before the plugin unloads
        if (askUser(p, std::move(question), [ctx, shared](bool yes) { answered(*ctx, *shared, yes); }) == 0) {
            for (const Removal& r : *shared) r.work->resolve(nullptr);  // a stopped call's code can't ask
        }
    } catch (...) {
        // Never into QuickJS's C frames; the removals' works end with their call.
    }
    return JS_UNDEFINED;
}

// n.cancel(changeId)
JSValue cancelNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string id;
    if (!text(c, argc, argv, 0, id)) return JS_EXCEPTION;
    return JS_NewBool(c, libraryOf(p).cancelPluginChange(p.manifest.id, id));
}

// n.pending(): this plugin's unsent changes as zk.d.ts's PendingChange.
JSValue pendingNative(JSContext* c, JSValueConst, int, JSValueConst*) {
    PluginCtx& p = ctxOf(c);
    json out = json::array();
    for (const PluginChange& ch : libraryOf(p).pendingPluginChanges(p.manifest.id)) {
        json j = {{"changeId", ch.id}, {"requested", isoOf(ch.createdMs)}, {"kind", kindName(ch.kind)}, {"key", ch.itemKey}};
        switch (ch.kind) {
            case Kind::CreateItem: {
                json v = json::parse(ch.value, nullptr, false);
                if (!v.is_object()) v = json::object();
                j["fields"] = v.contains("fields") ? v["fields"] : json::object();
                j["collections"] = v.contains("collections") ? v["collections"] : json::array();
                j["tags"] = v.contains("tags") ? v["tags"] : json::array();
                break;
            }
            case Kind::AddToCollection:
            case Kind::RemoveFromCollection: j["collection"] = ch.value; break;
            case Kind::AddTag:
            case Kind::RemoveTag: j["tag"] = ch.value; break;
            case Kind::AddNote: j["itemKey"] = ch.parentKey; break;
            default: break;
        }
        out.push_back(std::move(j));
    }
    return toJs(c, out);
}

// ---- What plugins see ---------------------------------------------------------------------

const char kZoteroJs[] = R"JS((function (n) {
"use strict";
const FIELDS = ["type", "title", "creators", "date", "publication", "doi", "arxiv", "isbn", "url", "abstract", "extra"];

function str(v, call, what) {
  if (typeof v !== "string" || v === "") throw new TypeError(`zk.zotero.${call}: ${what} must be a non-empty string`);
  return v;
}

function list(v, what) {
  if (v === undefined) return [];
  if (!Array.isArray(v) || v.some((x) => typeof x !== "string" || x === ""))
    throw new TypeError(`zk.zotero.createItem: ${what} must be an array of non-empty strings`);
  return v.slice();
}

function fieldsOf(f) {
  if (f === null || typeof f !== "object" || Array.isArray(f)) throw new TypeError("zk.zotero.createItem: the fields must be an object");
  const out = {};
  for (const k of Object.keys(f)) {
    if (!FIELDS.includes(k)) throw new TypeError(`zk.zotero.createItem: there is no field "${k}" (fields: ${FIELDS.join(", ")})`);
    if (f[k] === undefined) continue;
    if (k === "creators") {
      if (!Array.isArray(f.creators)) throw new TypeError("zk.zotero.createItem: creators must be an array");
      out.creators = f.creators.map((c) => {
        if (c === null || typeof c !== "object") throw new TypeError("zk.zotero.createItem: a creator must be an object");
        const o = {};
        for (const p of ["type", "first", "last", "name"]) {
          if (c[p] === undefined) continue;
          if (typeof c[p] !== "string") throw new TypeError(`zk.zotero.createItem: a creator's ${p} must be a string`);
          o[p] = c[p];
        }
        if (!o.name && !o.first && !o.last) throw new TypeError("zk.zotero.createItem: a creator needs a name, or first and last");
        return o;
      });
    } else {
      if (typeof f[k] !== "string") throw new TypeError(`zk.zotero.createItem: ${k} must be a string`);
      out[k] = f[k];
    }
  }
  str(out.type, "createItem", "type");
  str(out.title, "createItem", "title");
  return out;
}

return {
  async createItem(fields, opts) {
    if (opts !== undefined && (opts === null || typeof opts !== "object")) throw new TypeError("zk.zotero.createItem: the options must be an object");
    const o = opts || {};
    const collections = list(o.collections, "collections"), tags = list(o.tags, "tags");
    n.need("createItem", collections.length > 0, tags.length > 0);
    return n.add("createItem", JSON.stringify({ fields: fieldsOf(fields), collections, tags }));
  },
  async addToCollection(itemKey, collectionKey) {
    n.need("addToCollection");
    return n.add("addToCollection", str(itemKey, "addToCollection", "the item key"), str(collectionKey, "addToCollection", "the collection key"));
  },
  async addTag(itemKey, tag) {
    n.need("addTag");
    return n.add("addTag", str(itemKey, "addTag", "the item key"), str(tag, "addTag", "the tag"));
  },
  async addNote(itemKey, html) {
    n.need("addNote");
    if (typeof html !== "string") throw new TypeError("zk.zotero.addNote: the note must be a string of HTML");
    return n.add("addNote", str(itemKey, "addNote", "the item key"), html);
  },
  async trash(itemKey) {
    n.need("trash");
    return n.remove("trash", str(itemKey, "trash", "the item key"), "");
  },
  async removeFromCollection(itemKey, collectionKey) {
    n.need("removeFromCollection");
    return n.remove("removeFromCollection", str(itemKey, "removeFromCollection", "the item key"),
                    str(collectionKey, "removeFromCollection", "the collection key"));
  },
  async removeTag(itemKey, tag) {
    n.need("removeTag");
    return n.remove("removeTag", str(itemKey, "removeTag", "the item key"), str(tag, "removeTag", "the tag"));
  },
  async deleteNote(noteKey) {
    n.need("deleteNote");
    return n.remove("deleteNote", str(noteKey, "deleteNote", "the note key"), "");
  },
  async cancel(changeId) {
    if (typeof changeId !== "string") throw new TypeError("zk.zotero.cancel: the changeId must be a string");
    return n.cancel(changeId);
  },
  async pending() { return n.pending(); },
};
}))JS";

}  // namespace

void installZotero(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction<needNative>(c, natives.raw(), "need", 3);
    defineFunction<addNative>(c, natives.raw(), "add", 3);
    defineFunction<removeNative>(c, natives.raw(), "remove", 3);
    defineFunction<cancelNative>(c, natives.raw(), "cancel", 1);
    defineFunction<pendingNative>(c, natives.raw(), "pending", 0);

    const char* name = "<zk zotero>";
    js::Value fn(c, JS_Eval(c, kZoteroJs, sizeof kZoteroJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) {
        js::Value ex(c, JS_GetException(c));
        js::Value msg = ex.get("message");
        size_t len = 0;
        const char* s = JS_ToCStringLen(c, &len, msg.raw());
        std::string t = s ? std::string(s, len) : "?";
        if (s) JS_FreeCString(c, s);
        throw std::runtime_error(std::string(name) + ": " + t);
    }
    js::Value zotero = p.cx.call(fn, {natives});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
    JS_SetPropertyStr(c, p.zk.raw(), "zotero", JS_DupValue(c, zotero.raw()));
}

}  // namespace zk::plugins
