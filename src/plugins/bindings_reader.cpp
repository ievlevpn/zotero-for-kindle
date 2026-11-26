// zk.reader.open (0.9): a document opened in the app's reader as a preview, not added to the
// library. Checked here (when it may be called, the document's shape, https and the net permission
// for a url); the screen thread downloads and opens it (ReaderOpen, see host.h) and answers with
// PluginHost::answerReader, which settles the promise. Waiting for that is free, as waiting for the
// user's confirmation is.
#include "plugins/plugin_ctx.h"
#include "plugins/plugin_http.h"

namespace zk::plugins {
namespace {

constexpr size_t kMaxHtmlBytes = 10u << 20;
constexpr size_t kMaxTitleChars = 300;

std::string clipped(const std::string& s, size_t chars) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && n++ == chars) return s.substr(0, i) + "…";
    return s;
}

JSValue jsReaderOpen(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    constexpr const char* kShape = "zk.reader.open: expected { url, title? } or { html, title, baseUrl? }";
    std::string raw;
    if (argc > 0) {
        size_t len = 0;
        const char* s = JS_ToCStringLen(c, &len, argv[0]);
        if (!s) return JS_EXCEPTION;
        raw = sanitisedUtf8(s, len);
        JS_FreeCString(c, s);
    }
    const auto doc = nlohmann::json::parse(raw, nullptr, false);
    if (!doc.is_object()) return JS_ThrowTypeError(c, "%s", kShape);
    for (auto it = doc.begin(); it != doc.end(); ++it)
        if (it.key() != "url" && it.key() != "html" && it.key() != "title" && it.key() != "baseUrl" && it.key() != "save")
            return JS_ThrowTypeError(c, "zk.reader.open: unknown field \"%s\"", it.key().c_str());
    const auto str = [&](const char* k) -> const nlohmann::json* {
        auto it = doc.find(k);
        return it == doc.end() || it->is_null() ? nullptr : &*it;
    };
    const auto* url = str("url");
    const auto* html = str("html");
    const auto* title = str("title");
    const auto* base = str("baseUrl");
    std::string saveMode;
    if (auto it = doc.find("save"); it != doc.end() && !it->is_null()) {
        if (it->is_boolean()) {
            if (it->get<bool>()) saveMode = "allow";
        } else if (it->is_string() && it->get<std::string>() == "prompt") {
            saveMode = "prompt";
        } else {
            return JS_ThrowTypeError(c, "zk.reader.open: `save` must be a boolean or \"prompt\"");
        }
    }
    if ((!url) == (!html) || (url && !url->is_string()) || (html && !html->is_string()) || (title && !title->is_string()) ||
        (base && (!base->is_string() || url)))
        return JS_ThrowTypeError(c, "%s", kShape);
    const std::string shownTitle = title ? clipped(oneLine(title->get<std::string>()), kMaxTitleChars) : "";
    nlohmann::json out = nlohmann::json::object();
    if (html) {
        if (shownTitle.empty()) return JS_ThrowTypeError(c, "zk.reader.open: an html document needs a `title`");
        if (html->get_ref<const std::string&>().size() > kMaxHtmlBytes)
            return JS_ThrowRangeError(c, "zk.reader.open: the html is over 10 MB");
    }

    uint64_t screen = 0;
    if (const std::string err = userAnswerCheck(p, "zk.reader.open", &screen); !err.empty())
        return JS_ThrowPlainError(c, "%s", err.c_str());

    if (url) {
        const std::string u = url->get<std::string>();
        if (auto e = checkFetchUrl(p.manifest, u, webdavHostOf(p))) {
            if (e->kind == FetchError::Kind::Permission) return throwPermissionError(p, e->detail, "zk.reader.open(\"" + u + "\")");
            std::string m = e->what();
            if (m.rfind("zk.fetch", 0) == 0) m = "zk.reader.open" + m.substr(8);
            return JS_ThrowPlainError(c, "%s", m.c_str());
        }
        out["url"] = u;
        if (!shownTitle.empty()) out["title"] = shownTitle;
    } else {
        out["html"] = html->get<std::string>();
        out["title"] = shownTitle;
        if (base && !base->get_ref<const std::string&>().empty()) out["baseUrl"] = base->get<std::string>();
    }
    if (!saveMode.empty()) out["save"] = saveMode;

    JSValue promise = JS_UNDEFINED;
    auto work = startWork(p, &promise);
    if (work->cancelled()) return promise;  // already rejected (the call was stopped)
    PluginHostEvent ev{PluginHostEvent::Kind::ReaderOpen, "", "", "", {}, 0, ""};
    ev.instance = screen;
    ev.value = std::move(out);
    const uint64_t id = waitForUi(p, std::move(ev), [work](const std::string& error) {
        if (error.empty()) work->finish([](JSContext*) { return JS_UNDEFINED; });
        else work->reject(error);
    });
    if (id == 0) work->reject("zk.reader.open: the call was stopped");
    return promise;
}

constexpr char kReaderJs[] = R"JS((function (n, zk) {
"use strict";
zk.reader = Object.freeze({
  open(doc) {
    let s;
    try { s = JSON.stringify(doc); } catch (e) { s = undefined; }
    return n.open(typeof s === "string" ? s : "null");
  },
});
}))JS";

}  // namespace

void installReader(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction<jsReaderOpen>(c, natives.raw(), "open", 1);
    const char* name = "<zk reader>";
    js::Value fn(c, JS_Eval(c, kReaderJs, sizeof kReaderJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) throw std::runtime_error(std::string(name) + ": the prelude doesn't compile");
    p.cx.call(fn, {natives, p.zk});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
}

}  // namespace zk::plugins
