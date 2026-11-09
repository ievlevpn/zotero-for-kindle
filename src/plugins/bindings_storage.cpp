// zk.storage (a private key-value store in <plugin>/storage.db) and zk.files (text files in
// <plugin>/data/). Both are fast local I/O, done right inside the native on the plugin thread.
//
// The natives are plain and synchronous; the JavaScript below is what plugins see: promises, and
// the one per-plugin queue every storage write goes through (set, delete, update), in the order
// asked. An update's `fn` is the only thing in the queue that can take long, so each update holds
// a "slot": host work owned by the call that asked for it. If that call is stopped (LimitError), the
// slot rejects and the update fails, so the queue moves on rather than hanging. Values are encoded
// to JSON text when set() is called (or when an update's fn returns), never later.

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "plugins/plugin_ctx.h"
#include "store/db.h"

namespace zk::plugins {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxValue = 1u << 20;       // one stored value (JSON text)
constexpr size_t kMaxStore = 16u << 20;      // all keys and values of one plugin
constexpr size_t kMaxFile = 16u << 20;       // one file written or read through zk.files

struct StorageState {
    std::unique_ptr<Db> db;
    std::map<uint64_t, std::shared_ptr<Work>> slots;  // by Work id: one per update not yet settled
    uint64_t gen = 0;            // bumped by a reset: queued writes of the old generation fail
    int64_t total = -1;          // bytes of all keys and values; -1: not counted yet (after open)
};

StorageState& stateOf(PluginCtx& p) {
    if (!p.storage) p.storage = std::make_shared<StorageState>();
    return *static_cast<StorageState*>(p.storage.get());
}

Db& dbOf(PluginCtx& p) {
    StorageState& st = stateOf(p);
    if (!st.db) {
        std::error_code ec;
        fs::create_directories(p.dir, ec);
        auto db = std::make_unique<Db>(Db::open(p.dir + "/storage.db"));
        db->exec("CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
        st.db = std::move(db);
        st.total = -1;
    }
    return *st.db;
}

// The running total of key and value bytes: counted once after opening, then adjusted.
int64_t& totalOf(PluginCtx& p) {
    Db& db = dbOf(p);
    StorageState& st = stateOf(p);
    if (st.total < 0) {
        Stmt q = db.prepare("SELECT COALESCE(SUM(LENGTH(CAST(key AS BLOB)) + LENGTH(CAST(value AS BLOB))), 0) FROM kv");
        st.total = q.step() ? q.integer(0) : 0;
    }
    return st.total;
}

JSValue fail(JSContext* c, const std::string& message) { return JS_ThrowPlainError(c, "%s", message.c_str()); }

// UTF-8 with every invalid byte replaced by U+FFFD.
std::string sanitised(const char* s, size_t n) {
    std::string out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        size_t len = b < 0x80 ? 1 : (b >= 0xC2 && b <= 0xDF) ? 2 : (b >= 0xE0 && b <= 0xEF) ? 3 : (b >= 0xF0 && b <= 0xF4) ? 4 : 0;
        bool ok = len != 0 && i + len <= n;
        if (ok) {
            for (size_t k = 1; k < len; ++k) ok = ok && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        }
        if (ok && len == 3) {
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            ok = !(b == 0xE0 && b1 < 0xA0) && !(b == 0xED && b1 >= 0xA0);  // overlong, surrogates
        }
        if (ok && len == 4) {
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            ok = !(b == 0xF0 && b1 < 0x90) && !(b == 0xF4 && b1 >= 0x90);
        }
        if (ok) {
            out.append(s + i, len);
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

bool toText(JSContext* c, JSValueConst v, std::string& out) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) return false;
    out = sanitised(s, len);
    JS_FreeCString(c, s);
    return true;
}

bool needString(JSContext* c, int argc, JSValueConst* argv, int i, const char* what, std::string& out) {
    if (i >= argc || !JS_IsString(argv[i])) {
        JS_ThrowTypeError(c, "%s must be a string", what);
        return false;
    }
    return toText(c, argv[i], out);
}

// ---- storage ------------------------------------------------------------------------------

JSValue stGet(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string key;
    if (!needString(c, argc, argv, 0, "zk.storage: the key", key)) return JS_EXCEPTION;
    Stmt st = dbOf(ctxOf(c)).prepare("SELECT value FROM kv WHERE key = ?");
    st.bind(1, key);
    if (!st.step()) return JS_UNDEFINED;
    const std::string text = st.text(0);
    JSValue v = JS_ParseJSON(c, text.data(), text.size(), "<storage>");
    if (JS_IsException(v)) {
        JS_FreeValue(c, JS_GetException(c));
        return fail(c, "zk.storage: the value stored under \"" + key + "\" can't be read (damaged)");
    }
    return v;
}

JSValue stKeys(JSContext* c, JSValueConst, int, JSValueConst*) {
    Stmt st = dbOf(ctxOf(c)).prepare("SELECT key FROM kv ORDER BY key");
    JSValue arr = JS_NewArray(c);
    uint32_t i = 0;
    while (st.step()) {
        const std::string k = st.text(0);
        JS_SetPropertyUint32(c, arr, i++, JS_NewStringLen(c, k.data(), k.size()));
    }
    return arr;
}

// Value -> the JSON text to store, now (the caller's value may change later, and a value that isn't
// JSON is refused at the call). Throws a JS error otherwise.
JSValue stEncode(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string key;
    if (!needString(c, argc, argv, 0, "zk.storage: the key", key)) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(c, "zk.storage: a value is needed");
    JSValue json = JS_JSONStringify(c, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) return JS_EXCEPTION;
    if (!JS_IsString(json)) {
        JS_FreeValue(c, json);
        return JS_ThrowTypeError(c, "zk.storage: the value for \"%s\" isn't JSON (undefined, a function or a symbol)", key.c_str());
    }
    std::string text;
    const bool ok = toText(c, json, text);
    JS_FreeValue(c, json);
    if (!ok) return JS_EXCEPTION;
    if (text.size() > kMaxValue)
        return fail(c, "zk.storage: the value for \"" + key + "\" is " + std::to_string(text.size() / 1024) +
                           " KB; one value may be at most 1 MB");
    return JS_NewStringLen(c, text.data(), text.size());
}

JSValue stPut(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string key, text;
    if (!needString(c, argc, argv, 0, "zk.storage: the key", key) || !needString(c, argc, argv, 1, "zk.storage: the text", text))
        return JS_EXCEPTION;
    PluginCtx& p = ctxOf(c);
    Db& db = dbOf(p);
    int64_t& total = totalOf(p);
    int64_t old = 0;
    {
        Stmt st = db.prepare("SELECT LENGTH(CAST(key AS BLOB)) + LENGTH(CAST(value AS BLOB)) FROM kv WHERE key = ?");
        st.bind(1, key);
        if (st.step()) old = st.integer(0);
    }
    const int64_t mine = static_cast<int64_t>(key.size() + text.size());
    if (static_cast<size_t>(total - old + mine) > kMaxStore)
        return fail(c, "zk.storage: storing \"" + key + "\" would take this plugin's storage past 16 MB (it holds " +
                           std::to_string((total - old) / 1024) + " KB of other data); delete something first");
    Stmt up = db.prepare("INSERT INTO kv(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    up.bind(1, key).bind(2, text);
    up.step();
    total += mine - old;
    return JS_UNDEFINED;
}

JSValue stDel(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string key;
    if (!needString(c, argc, argv, 0, "zk.storage: the key", key)) return JS_EXCEPTION;
    PluginCtx& p = ctxOf(c);
    Db& db = dbOf(p);
    int64_t& total = totalOf(p);
    bool had = false;
    int64_t size = 0;
    {
        Stmt st = db.prepare("SELECT LENGTH(CAST(key AS BLOB)) + LENGTH(CAST(value AS BLOB)) FROM kv WHERE key = ?");
        st.bind(1, key);
        had = st.step();
        if (had) size = st.integer(0);
    }
    if (had) {
        Stmt d = db.prepare("DELETE FROM kv WHERE key = ?");
        d.bind(1, key);
        d.step();
        total -= size;  // only once the row is really gone
    }
    return JS_NewBool(c, had);
}

JSValue stGen(JSContext* c, JSValueConst, int, JSValueConst*) {
    return JS_NewInt64(c, static_cast<int64_t>(stateOf(ctxOf(c)).gen));
}

JSValue stInCleanup(JSContext* c, JSValueConst, int, JSValueConst*) { return JS_NewBool(c, currentCall(ctxOf(c)).cleanup); }

// A slot is host work owned by the call that asked for an update: if that call is stopped the slot
// rejects with its LimitError, and the update (waiting or running) fails instead of hanging.
JSValue stSlotStart(JSContext* c, JSValueConst, int, JSValueConst*) {
    PluginCtx& p = ctxOf(c);
    JSValue promise;
    auto work = startWork(p, &promise);
    stateOf(p).slots[work->id()] = work;
    JSValue pair = JS_NewArray(c);
    JS_SetPropertyUint32(c, pair, 0, JS_NewInt64(c, static_cast<int64_t>(work->id())));
    JS_SetPropertyUint32(c, pair, 1, promise);
    return pair;
}

bool slotId(JSContext* c, int argc, JSValueConst* argv, uint64_t& id) {
    int64_t v = 0;
    if (argc < 1 || JS_ToInt64(c, &v, argv[0])) {
        JS_FreeValue(c, JS_GetException(c));
        return false;
    }
    id = static_cast<uint64_t>(v);
    return true;
}

JSValue stSlotCancelled(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    uint64_t id;
    auto& slots = stateOf(ctxOf(c)).slots;
    auto it = slotId(c, argc, argv, id) ? slots.find(id) : slots.end();
    return JS_NewBool(c, it != slots.end() && it->second->cancelled());
}

JSValue stSlotEnd(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    uint64_t id;
    auto& slots = stateOf(ctxOf(c)).slots;
    auto it = slotId(c, argc, argv, id) ? slots.find(id) : slots.end();
    if (it != slots.end()) {
        it->second->resolve(nullptr);
        slots.erase(it);
    }
    return JS_UNDEFINED;
}

// ---- files --------------------------------------------------------------------------------

struct Resolved {
    fs::path full;
    bool isRoot = false;
};

// `rel` as written by the plugin -> a path inside data/. Throws a JS Error (and returns false) if it
// leaves the folder, lexically or through a symlink.
bool resolveInData(JSContext* c, PluginCtx& p, const std::string& rel, bool allowRoot, Resolved& out) {
    auto outside = [&] { fail(c, "zk.files: \"" + rel + "\" is outside the plugin's data folder"); return false; };
    if (rel.find('\0') != std::string::npos) return outside();
    if (!rel.empty() && rel[0] == '/') return outside();
    fs::path clean;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        const std::string seg = rel.substr(i, j - i);
        if (seg == "..") return outside();
        if (!seg.empty() && seg != ".") clean /= seg;
        i = j + 1;
    }
    if (clean.empty() && !allowRoot) {
        fail(c, "zk.files: a path is needed");
        return false;
    }
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(p.dataDir, ec);
    if (ec) {
        fail(c, "zk.files: the data folder can't be reached: " + ec.message());
        return false;
    }
    const fs::path full = fs::weakly_canonical(base / clean, ec);
    if (ec) {
        fail(c, "zk.files: \"" + rel + "\" can't be resolved: " + ec.message());
        return false;
    }
    const fs::path inside = full.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") return outside();
    out.full = full;
    out.isRoot = inside == ".";
    return true;
}

JSValue flRead(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string rel;
    Resolved r;
    if (!needString(c, argc, argv, 0, "zk.files: the path", rel) || !resolveInData(c, p, rel, false, r)) return JS_EXCEPTION;
    std::error_code ec;
    if (!fs::is_regular_file(r.full, ec)) return fail(c, "zk.files: \"" + rel + "\" doesn't exist (or isn't a file)");
    const auto size = fs::file_size(r.full, ec);
    if (ec) return fail(c, "zk.files: \"" + rel + "\" can't be read: " + ec.message());
    if (size > kMaxFile) return fail(c, "zk.files: \"" + rel + "\" is over 16 MB, too big to read as text");
    std::ifstream in(r.full, std::ios::binary);
    if (!in) return fail(c, "zk.files: \"" + rel + "\" can't be opened");
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string bytes = sanitised(ss.str().data(), ss.str().size());
    return JS_NewStringLen(c, bytes.data(), bytes.size());
}

JSValue flWrite(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string rel, text;
    Resolved r;
    if (!needString(c, argc, argv, 0, "zk.files: the path", rel) || !resolveInData(c, p, rel, false, r)) return JS_EXCEPTION;
    if (!needString(c, argc, argv, 1, "zk.files: the text", text)) return JS_EXCEPTION;
    if (text.size() > kMaxFile)
        return fail(c, "zk.files: \"" + rel + "\" would be " + std::to_string(text.size() >> 20) + " MB; one file may be at most 16 MB");
    std::error_code ec;
    fs::create_directories(r.full.parent_path(), ec);
    if (ec) return fail(c, "zk.files: can't create the folder for \"" + rel + "\": " + ec.message());
    if (fs::is_directory(r.full, ec)) return fail(c, "zk.files: \"" + rel + "\" is a folder");
    const fs::path tmp = r.full.string() + ".zk-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return fail(c, "zk.files: \"" + rel + "\" couldn't be written (disk full?)");
        }
    }
    fs::rename(tmp, r.full, ec);
    if (ec) {
        std::error_code ignore;
        fs::remove(tmp, ignore);
        return fail(c, "zk.files: \"" + rel + "\" couldn't be written: " + ec.message());
    }
    return JS_UNDEFINED;
}

JSValue flList(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string rel;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !needString(c, argc, argv, 0, "zk.files: the folder", rel)) return JS_EXCEPTION;
    Resolved r;
    if (!resolveInData(c, p, rel, true, r)) return JS_EXCEPTION;
    std::error_code ec;
    JSValue arr = JS_NewArray(c);
    if (!fs::exists(r.full, ec)) return arr;
    if (!fs::is_directory(r.full, ec)) {
        JS_FreeValue(c, arr);
        return fail(c, "zk.files: \"" + rel + "\" isn't a folder");
    }
    std::vector<std::string> names;
    for (fs::directory_iterator it(r.full, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        std::string n = it->path().filename().string();
        if (it->is_directory(e2)) n += '/';
        names.push_back(std::move(n));
    }
    std::sort(names.begin(), names.end());
    uint32_t i = 0;
    for (const auto& n : names) JS_SetPropertyUint32(c, arr, i++, JS_NewStringLen(c, n.data(), n.size()));
    return arr;
}

JSValue flRemove(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    std::string rel;
    Resolved r;
    if (!needString(c, argc, argv, 0, "zk.files: the path", rel) || !resolveInData(c, p, rel, false, r)) return JS_EXCEPTION;
    std::error_code ec;
    if (!fs::exists(r.full, ec)) return JS_FALSE;
    fs::remove_all(r.full, ec);
    if (ec) return fail(c, "zk.files: \"" + rel + "\" couldn't be removed: " + ec.message());
    return JS_TRUE;
}

// ---- What plugins see ---------------------------------------------------------------------

const char kStorageJs[] = R"JS((function (n, setT) {
"use strict";
let tail = Promise.resolve();
let inFn = false;  // true while an update's fn runs its synchronous part

const reset = () => new Error("zk.storage was reset (Settings → Plugins → Reset its data) before this write ran");

// Runs `task` after everything queued before it; a failed task never blocks the next.
function enqueue(what, task) {
  if (inFn) return Promise.reject(new Error(`zk.storage.${what} can't be called inside another update's function`));
  const gen = n.gen();
  const run = tail.then(async () => {
    // Never start a task inside a stopped call's cleanup (host work started there is rejected with
    // that call's LimitError, and this task isn't that call's): a timer set there is a call of its own.
    if (n.inCleanup()) await new Promise((r) => setT(r, 0));
    if (n.gen() !== gen) throw reset();
    return task(gen);
  });
  tail = run.then(() => {}, () => {});
  return run;
}

const storage = {
  async get(key) { return n.get(key); },
  async keys() { return n.keys(); },
  set(key, value) {
    if (inFn) return enqueue("set");
    let text;
    try { text = n.encode(key, value); } catch (e) { return Promise.reject(e); }  // now, not when its turn comes
    return enqueue("set", () => { n.put(key, text); });
  },
  delete(key) {
    return enqueue("delete", () => n.del(key));
  },
  update(key, fn) {
    if (inFn) return enqueue("update");
    // The slot belongs to the call that asks (this one), not to whichever call runs the queue later.
    const [slotId, slot] = n.slotStart();
    slot.catch(() => {});
    const run = enqueue("update", async (gen) => {
      if (typeof fn !== "function") throw new TypeError("zk.storage.update: fn must be a function");
      if (n.slotCancelled(slotId)) await slot;  // the asker was stopped while this waited: LimitError
      const old = n.get(key);
      let result;
      let r;
      inFn = true;
      try { r = fn(old); } finally { inFn = false; }
      result = await Promise.race([r, slot]);
      if (n.gen() !== gen) throw reset();
      if (result === undefined) n.del(key);
      else n.put(key, n.encode(key, result));  // encoded as fn returned it, then stored
      return n.get(key);
    });
    const end = () => n.slotEnd(slotId);
    run.then(end, end);
    return run;
  },
};

const files = {
  async read(path) { return n.read(path); },
  async write(path, text) { n.write(path, text); },
  async list(dir) { return n.list(dir); },
  async remove(path) { return n.remove(path); },
};

return { storage, files };
}))JS";

}  // namespace

void installStorage(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction<stGet>(c, natives.raw(), "get", 1);
    defineFunction<stKeys>(c, natives.raw(), "keys", 0);
    defineFunction<stEncode>(c, natives.raw(), "encode", 2);
    defineFunction<stPut>(c, natives.raw(), "put", 2);
    defineFunction<stDel>(c, natives.raw(), "del", 1);
    defineFunction<stGen>(c, natives.raw(), "gen", 0);
    defineFunction<stInCleanup>(c, natives.raw(), "inCleanup", 0);
    defineFunction<stSlotStart>(c, natives.raw(), "slotStart", 0);
    defineFunction<stSlotCancelled>(c, natives.raw(), "slotCancelled", 1);
    defineFunction<stSlotEnd>(c, natives.raw(), "slotEnd", 1);
    defineFunction<flRead>(c, natives.raw(), "read", 1);
    defineFunction<flWrite>(c, natives.raw(), "write", 2);
    defineFunction<flList>(c, natives.raw(), "list", 1);
    defineFunction<flRemove>(c, natives.raw(), "remove", 1);

    JSValue global = JS_GetGlobalObject(c);
    js::Value setT(c, JS_GetPropertyStr(c, global, "setTimeout"));
    JS_FreeValue(c, global);

    const char* name = "<zk storage>";
    js::Value fn(c, JS_Eval(c, kStorageJs, sizeof kStorageJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) {
        js::Value ex(c, JS_GetException(c));
        js::Value msg = ex.get("message");
        size_t len = 0;
        const char* s = JS_ToCStringLen(c, &len, msg.raw());
        std::string text = s ? std::string(s, len) : "?";
        if (s) JS_FreeCString(c, s);
        throw std::runtime_error(std::string(name) + ": " + text);
    }
    js::Value made = p.cx.call(fn, {natives, setT});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
    js::Value storage = made.get("storage"), files = made.get("files");
    JS_SetPropertyStr(c, p.zk.raw(), "storage", JS_DupValue(c, storage.raw()));
    JS_SetPropertyStr(c, p.zk.raw(), "files", JS_DupValue(c, files.raw()));
}

void resetPluginData(const std::string& pluginDir, PluginCtx* loaded) {
    if (loaded) {
        StorageState& st = stateOf(*loaded);
        ++st.gen;
        st.db.reset();  // closed; reopened (empty) at the next use
    }
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm", "-journal"}) fs::remove(pluginDir + "/storage.db" + suffix, ec);
    const fs::path data = fs::path(pluginDir) / "data";
    if (fs::is_symlink(data, ec)) {
        fs::remove(data, ec);  // the link itself; never what it points to
    } else if (fs::is_directory(data, ec))
        for (fs::directory_iterator it(data, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            fs::remove_all(it->path(), e2);
        }
}

}  // namespace zk::plugins
