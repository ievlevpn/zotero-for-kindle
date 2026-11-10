// zk.selectionAction, zk.itemAction: entry points the app shows; checked and recorded (kept in the
// plugin's run state, listed in PluginStatus). zk.screen is bindings_ui.cpp's.
#include <regex>
#include <stdexcept>

#include "plugins/plugin_ctx.h"

namespace zk::plugins {
namespace {

constexpr size_t kTitleChars = 40;

bool validId(const std::string& id) {
    static const std::regex re("^[a-z0-9]+(-[a-z0-9]+)*$");
    return id.size() <= 64 && std::regex_match(id, re);
}

size_t characters(const std::string& s) {
    size_t n = 0;
    for (unsigned char ch : s) n += (ch & 0xC0) != 0x80;
    return n;
}

std::string text(JSContext* c, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) {
        JS_FreeValue(c, JS_GetException(c));
        return "";
    }
    std::string out = sanitisedUtf8(s, len);
    JS_FreeCString(c, s);
    return out;
}

const char* idRule = "lowercase letters, digits and dashes, at most 64 characters";

}  // namespace

bool checkEntryId(JSContext* c, JSValueConst v, const char* fn, const std::string& kind, std::string* id) {
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(c, "%s: `id` must be a string (%s)", fn, idRule);
        return false;
    }
    *id = text(c, v);
    if (!validId(*id)) {
        JS_ThrowTypeError(c, "%s: `id` must be %s: \"%s\"", fn, idRule, id->c_str());
        return false;
    }
    if (hasEntry(ctxOf(c), kind, *id)) {
        JS_ThrowTypeError(c, "%s: there is already one with the `id` \"%s\"; ids must be unique", fn, id->c_str());
        return false;
    }
    return true;
}

namespace {

JSValue action(JSContext* c, JSValueConst arg, const char* fn, const char* kind) {
    if (!JS_IsObject(arg)) return JS_ThrowTypeError(c, "%s: expected an object { id, title, run }", fn);
    JSValue idV = JS_GetPropertyStr(c, arg, "id"), titleV = JS_GetPropertyStr(c, arg, "title"),
            runV = JS_GetPropertyStr(c, arg, "run");
    JSValue result = JS_UNDEFINED;
    std::string id, title;
    if (!checkEntryId(c, idV, fn, kind, &id)) result = JS_EXCEPTION;
    else if (!JS_IsString(titleV) || (title = oneLine(text(c, titleV))).empty() || characters(title) > kTitleChars)
        result = JS_ThrowTypeError(c, "%s: `title` must be a non-empty string (at most %zu characters)", fn, kTitleChars);
    else if (!JS_IsFunction(c, runV)) result = JS_ThrowTypeError(c, "%s: `run` must be a function", fn);
    else addEntry(ctxOf(c), kind, id, title, js::Value(c, JS_DupValue(c, runV)));
    JS_FreeValue(c, idV);
    JS_FreeValue(c, titleV);
    JS_FreeValue(c, runV);
    return result;
}

JSValue jsSelectionAction(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    return action(c, argc > 0 ? argv[0] : JS_UNDEFINED, "zk.selectionAction", "selectionAction");
}

JSValue jsItemAction(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    return action(c, argc > 0 ? argv[0] : JS_UNDEFINED, "zk.itemAction", "itemAction");
}

// Runs an action's `run` and turns what it returned into JSON text inside the call (a toJSON or a
// getter runs in the call's budget): "" for nothing, else {text, actions?} (a string is {text}).
constexpr char kRunnerJs[] = R"JS((run, target, again) => Promise.resolve()
  .then(() => run(target, again))
  .then((r) => {
    if (r === undefined || r === null) return "";
    const s = JSON.stringify(typeof r === "string" ? { text: r } : r);
    return typeof s === "string" ? s : "null";
  }))JS";

}  // namespace

void installEntry(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    defineFunction<jsSelectionAction>(c, p.zk.raw(), "selectionAction", 1);
    defineFunction<jsItemAction>(c, p.zk.raw(), "itemAction", 1);
    const char* name = "<zk actions>";
    p.actionRunner = js::Value(c, JS_Eval(c, kRunnerJs, sizeof kRunnerJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(p.actionRunner.raw())) {
        js::Value ex(c, JS_GetException(c));
        throw std::runtime_error(std::string(name) + ": " + text(c, ex.raw()));
    }
}

}  // namespace zk::plugins
