// zk.screen and zk.ui: a plugin's screens. The helpers (zk.ui.column, …) only make plain objects
// (a JS prelude); the host keeps the open screens and runs render/onEvent (host.cpp, "Screens").
// render runs through a small runner that turns its result into JSON text inside the call, so
// the host never runs plugin code (a toJSON, a getter) outside a budget.
#include "plugins/plugin_ctx.h"

namespace zk::plugins {
namespace {

constexpr size_t kTitleChars = 40;

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

// undefined → nullopt; otherwise the JSON text the prelude made (always valid JSON).
std::optional<nlohmann::json> jsonArg(JSContext* c, int argc, JSValueConst* argv, int i) {
    if (argc <= i || JS_IsUndefined(argv[i])) return std::nullopt;
    auto j = nlohmann::json::parse(text(c, argv[i]), nullptr, false);
    if (j.is_discarded()) return nlohmann::json(nullptr);
    return j;
}

JSValue jsScreen(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    std::string id;
    if (!checkEntryId(c, argc > 0 ? argv[0] : JS_UNDEFINED, "zk.screen", "screen", &id)) return JS_EXCEPTION;
    JSValueConst spec = argc > 1 ? argv[1] : JS_UNDEFINED;
    if (!JS_IsObject(spec) || JS_IsFunction(c, spec))
        return JS_ThrowTypeError(c, "zk.screen: expected zk.screen(id, { title, render, onEvent?, main? })");
    js::Value titleV(c, JS_GetPropertyStr(c, spec, "title")), renderV(c, JS_GetPropertyStr(c, spec, "render")),
        eventV(c, JS_GetPropertyStr(c, spec, "onEvent")), mainV(c, JS_GetPropertyStr(c, spec, "main"));
    std::string title;
    if (!JS_IsString(titleV.raw()) || (title = oneLine(text(c, titleV.raw()))).empty() || characters(title) > kTitleChars)
        return JS_ThrowTypeError(c, "zk.screen: `title` must be a non-empty string (at most %zu characters)", kTitleChars);
    if (!JS_IsFunction(c, renderV.raw())) return JS_ThrowTypeError(c, "zk.screen: `render` must be a function");
    if (!JS_IsUndefined(eventV.raw()) && !JS_IsFunction(c, eventV.raw()))
        return JS_ThrowTypeError(c, "zk.screen: `onEvent` must be a function");
    if (!JS_IsUndefined(mainV.raw()) && !JS_IsBool(mainV.raw()))
        return JS_ThrowTypeError(c, "zk.screen: `main` must be true or false");
    addScreen(ctxOf(c), id, title, JS_ToBool(c, mainV.raw()) == 1, renderV, eventV);
    return JS_UNDEFINED;
}

JSValue jsOpen(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    const std::string err = uiOpen(ctxOf(c), argc > 0 ? text(c, argv[0]) : "", jsonArg(c, argc, argv, 1));
    if (!err.empty()) return JS_ThrowPlainError(c, "%s", err.c_str());
    return JS_UNDEFINED;
}

JSValue jsClose(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    const std::string err = uiClose(ctxOf(c), jsonArg(c, argc, argv, 0));
    if (!err.empty()) return JS_ThrowPlainError(c, "%s", err.c_str());
    return JS_UNDEFINED;
}

JSValue jsRefresh(JSContext* c, JSValueConst, int, JSValueConst*) {
    uiRefresh(ctxOf(c));
    return JS_UNDEFINED;
}

constexpr char kUiJs[] = R"JS((function (n, zk) {
"use strict";
const json = (v, what) => {
  if (v === undefined) return undefined;
  const s = JSON.stringify(v);
  if (typeof s !== "string") throw new TypeError(what + " must be plain JSON data");
  return s;
};
zk.ui = Object.freeze({
  column: (children, opts) => ({ kind: "column", children, ...opts }),
  row: (children, opts) => ({ kind: "row", children, ...opts }),
  text: (text, opts) => ({ kind: "text", text, ...opts }),
  card: (card) => ({ kind: "card", ...card }),
  deck: (deck) => ({ kind: "deck", ...deck }),
  list: (list) => ({ kind: "list", ...list }),
  search: (search) => ({ kind: "search", ...search }),
  field: (field) => ({ kind: "field", ...field }),
  chips: (chips) => ({ kind: "chips", ...chips }),
  button: (button) => ({ kind: "button", ...button }),
  buttons: (buttons) => ({ kind: "buttons", buttons }),
  divider: () => ({ kind: "divider" }),
  space: (size) => (size === undefined ? { kind: "space" } : { kind: "space", size }),
  notice: (text) => ({ kind: "notice", text }),
  open(screenId, params) {
    if (typeof screenId !== "string") throw new TypeError("zk.ui.open: the screen id must be a string");
    n.open(screenId, json(params, "zk.ui.open: params"));
  },
  close(result) { n.close(json(result, "zk.ui.close: the result")); },
  refresh() { n.refresh(); },
});
return (render, params) => Promise.resolve()
  .then(() => render(params))
  .then((tree) => { const s = JSON.stringify(tree === undefined ? null : tree); return typeof s === "string" ? s : "null"; });
}))JS";

}  // namespace

void installUi(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    defineFunction<jsScreen>(c, p.zk.raw(), "screen", 2);
    js::Value natives(c, JS_NewObject(c));
    defineFunction<jsOpen>(c, natives.raw(), "open", 2);
    defineFunction<jsClose>(c, natives.raw(), "close", 1);
    defineFunction<jsRefresh>(c, natives.raw(), "refresh", 0);
    const char* name = "<zk ui>";
    js::Value fn(c, JS_Eval(c, kUiJs, sizeof kUiJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) {
        js::Value ex(c, JS_GetException(c));
        throw std::runtime_error(std::string(name) + ": " + text(c, ex.raw()));
    }
    p.uiRender = p.cx.call(fn, {natives, p.zk});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
}

}  // namespace zk::plugins
