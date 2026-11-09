// zk.fetch: the plugin's request is checked into plain C++ data on the plugin thread, sent by the
// host's network workers through PluginHttp (the guard), and settled back on the plugin thread as a
// plain object the JavaScript below wraps in a web-like Response.

#include <stdexcept>

#include "plugins/plugin_ctx.h"
#include "plugins/plugin_http.h"

namespace zk::plugins {

namespace {

bool text(JSContext* c, JSValueConst v, const char* what, std::string& out) {
    if (!JS_IsString(v)) {
        JS_ThrowTypeError(c, "zk.fetch: %s must be a string", what);
        return false;
    }
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) return false;
    out = sanitisedUtf8(s, len);
    JS_FreeCString(c, s);
    return true;
}

// The response as a plain object: {status, url, headers: [[name, value]…] sorted by name, body}.
JSValue responseObject(JSContext* c, const HttpResponse& r) {
    js::Value o(c, JS_NewObject(c));
    if (JS_IsException(o.raw())) return JS_EXCEPTION;
    js::Value headers(c, JS_NewArray(c));
    if (JS_IsException(headers.raw())) return JS_EXCEPTION;
    uint32_t i = 0;
    for (const auto& [name, value] : r.headers) {  // a std::map: sorted by (lower-case) name
        const std::string n = sanitisedUtf8(name.data(), name.size()), v = sanitisedUtf8(value.data(), value.size());
        JSValue pair = JS_NewArray(c);
        if (JS_IsException(pair)) return JS_EXCEPTION;
        JS_SetPropertyUint32(c, pair, 0, JS_NewStringLen(c, n.data(), n.size()));
        JS_SetPropertyUint32(c, pair, 1, JS_NewStringLen(c, v.data(), v.size()));
        if (JS_SetPropertyUint32(c, headers.raw(), i++, pair) < 0) return JS_EXCEPTION;
    }
    const std::string url = sanitisedUtf8(r.url.data(), r.url.size());
    JSValue body = JS_NewArrayBufferCopy(c, reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size());
    if (JS_IsException(body)) return JS_EXCEPTION;
    if (JS_SetPropertyStr(c, o.raw(), "body", body) < 0 ||
        JS_SetPropertyStr(c, o.raw(), "status", JS_NewInt32(c, static_cast<int32_t>(r.status))) < 0 ||
        JS_SetPropertyStr(c, o.raw(), "url", JS_NewStringLen(c, url.data(), url.size())) < 0 ||
        JS_SetPropertyStr(c, o.raw(), "headers", JS_DupValue(c, headers.raw())) < 0)
        return JS_EXCEPTION;
    return JS_DupValue(c, o.raw());
}

Settle settleFor(FetchResult result) {
    if (result.response) {
        auto resp = std::make_shared<HttpResponse>(std::move(*result.response));
        return [resp](JSContext* c) { return responseObject(c, *resp); };
    }
    FetchError e = result.error ? *result.error : FetchError(FetchError::Kind::Network, "zk.fetch failed");
    return [e](JSContext* c) -> JSValue {
        PluginCtx& p = ctxOf(c);
        switch (e.kind) {
            case FetchError::Kind::Permission: return throwPermissionError(p, e.detail, e.what());
            case FetchError::Kind::Limit: {
                JSValue le = newLimitError(p, e.detail, e.what());
                if (JS_IsException(le)) return le;
                return JS_Throw(c, le);
            }
            default: return JS_ThrowPlainError(c, "%s", e.what());
        }
    };
}

// n.fetch(href, method, [[name, value]…], body | undefined, timeoutMs): the JS layer has shaped and
// checked the arguments; they are checked again here.
JSValue fetchNative(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(c, "zk.fetch: missing arguments");
    PluginCtx& p = ctxOf(c);
    HttpRequest req;
    if (!text(c, argv[0], "the URL", req.url) || !text(c, argv[1], "init.method", req.method)) return JS_EXCEPTION;
    if (!JS_IsArray(argv[2])) return JS_ThrowTypeError(c, "zk.fetch: init.headers must be a list");
    int64_t count = 0;
    if (JS_GetLength(c, argv[2], &count) < 0) return JS_EXCEPTION;
    for (int64_t i = 0; i < count; ++i) {
        js::Value pair(c, JS_GetPropertyInt64(c, argv[2], i));
        js::Value name(c, JS_GetPropertyUint32(c, pair.raw(), 0)), value(c, JS_GetPropertyUint32(c, pair.raw(), 1));
        std::string n, v;
        if (!text(c, name.raw(), "a header name", n) || !text(c, value.raw(), "a header value", v)) return JS_EXCEPTION;
        if (n.find_first_of(std::string(" \t\r\n:\0", 6)) != std::string::npos || n.empty() ||
            v.find_first_of(std::string("\r\n\0", 3)) != std::string::npos)
            return JS_ThrowTypeError(c, "zk.fetch: the header \"%s\" isn't valid", n.c_str());
        req.headers.emplace_back(std::move(n), std::move(v));
    }
    if (!JS_IsUndefined(argv[3])) {
        if (!JS_IsString(argv[3])) return JS_ThrowTypeError(c, "zk.fetch: init.body must be a string");
        size_t len = 0;
        const char* s = JS_ToCStringLen(c, &len, argv[3]);
        if (!s) return JS_EXCEPTION;
        if (len > kFetchMaxBytes) {
            JS_FreeCString(c, s);
            return JS_ThrowPlainError(c, "zk.fetch: the request body is over 10 MB");
        }
        req.body = sanitisedUtf8(s, len);
        JS_FreeCString(c, s);
    }
    int32_t timeoutMs = kFetchMaxMs;
    if (JS_ToInt32(c, &timeoutMs, argv[4]) < 0) return JS_EXCEPTION;
    if (timeoutMs < 1 || timeoutMs > kFetchMaxMs) timeoutMs = kFetchMaxMs;

    JSValue promise;
    auto work = startWork(p, &promise);
    if (JS_IsException(promise) || work->cancelled()) return promise;  // a stopped call's code: rejected already
    PluginNet* net = netOf(p);
    if (!net) {
        work->reject("zk.fetch: the network isn't available");
        return promise;
    }
    req.cancel = work->cancelFlag();  // the call stopped: the transfer aborts
    PluginNet::Job job;
    job.pluginId = p.manifest.id;
    job.manifest = p.manifest;
    job.request = std::move(req);
    job.timeoutMs = timeoutMs;
    job.done = [work](FetchResult r) { work->finish(settleFor(std::move(r))); };
    if (auto refused = net->submit(std::move(job))) {
        FetchResult r;
        r.error = std::move(refused);
        work->finish(settleFor(std::move(r)));
    }
    return promise;
}

// ---- What plugins see ---------------------------------------------------------------------

const char kFetchJs[] = R"JS((function (n, TextDecoder) {
"use strict";
const METHODS = ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE"];
const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const MAX_MS = 30000;
const utf8 = new TextDecoder();

function headerPairs(h) {
  if (h === undefined || h === null) return [];
  let pairs;
  if (Array.isArray(h)) {
    pairs = h.map((p) => {
      if (!Array.isArray(p) || p.length !== 2) throw new TypeError("zk.fetch: init.headers as a list must hold [name, value] pairs");
      return p;
    });
  } else if (typeof h === "object") {
    pairs = Object.entries(h);
  } else {
    throw new TypeError("zk.fetch: init.headers must be an object or a list of [name, value] pairs");
  }
  return pairs.map(([name, value]) => {
    name = String(name);
    value = String(value).replace(/^[ \t]+|[ \t]+$/g, "");
    if (!TOKEN.test(name)) throw new TypeError(`zk.fetch: "${name}" isn't a valid header name`);
    if (/[\r\n\0]/.test(value)) throw new TypeError(`zk.fetch: the ${name} header can't contain line breaks`);
    if (name.toLowerCase() === "cookie") throw new TypeError("zk.fetch: the Cookie header is not allowed (plugin requests carry no cookies)");
    return [name, value];
  });
}

class Headers {
  #m;
  constructor(pairs) { this.#m = new Map(pairs); }
  get(name) { const v = this.#m.get(String(name).toLowerCase()); return v === undefined ? null : v; }
  has(name) { return this.#m.has(String(name).toLowerCase()); }
  entries() { return this.#m.entries(); }
  [Symbol.iterator]() { return this.#m.entries(); }
}

class Response {
  #status; #url; #headers; #body;
  constructor(r) {
    this.#status = r.status;
    this.#url = r.url;
    this.#headers = new Headers(r.headers);
    this.#body = r.body;
  }
  get ok() { return this.#status >= 200 && this.#status <= 299; }
  get status() { return this.#status; }
  get url() { return this.#url; }
  get headers() { return this.#headers; }
  async text() { return utf8.decode(new Uint8Array(this.#body)); }
  async bytes() { return new Uint8Array(this.#body.slice(0)); }
  async json() { return JSON.parse(await this.text()); }
}

async function fetch(url, init) {
  if (init !== undefined && (init === null || typeof init !== "object")) throw new TypeError("zk.fetch: init must be an object");
  init = init || {};
  const href = new URL(String(url)).href;  // a TypeError if it isn't a URL
  const method = init.method === undefined ? "GET" : String(init.method).toUpperCase();
  if (!METHODS.includes(method)) throw new TypeError(`zk.fetch: the method "${init.method}" isn't allowed (${METHODS.join(", ")})`);
  const headers = headerPairs(init.headers);
  let body = init.body;
  if (body === null) body = undefined;
  if (body !== undefined && typeof body !== "string") throw new TypeError("zk.fetch: init.body must be a string");
  if (body !== undefined && (method === "GET" || method === "HEAD")) throw new TypeError(`zk.fetch: a ${method} request can't have a body`);
  let timeout = MAX_MS;
  if (init.timeout !== undefined) {
    if (typeof init.timeout !== "number" || !(init.timeout > 0)) throw new TypeError("zk.fetch: init.timeout must be a positive number of milliseconds");
    timeout = Math.min(Math.ceil(init.timeout), MAX_MS);  // more than 30 000 counts as 30 000
  }
  return new Response(await n.fetch(href, method, headers, body, timeout));
}

return fetch;
}))JS";

}  // namespace

void installFetch(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction<fetchNative>(c, natives.raw(), "fetch", 5);

    JSValue global = JS_GetGlobalObject(c);
    js::Value decoder(c, JS_GetPropertyStr(c, global, "TextDecoder"));
    JS_FreeValue(c, global);

    const char* name = "<zk fetch>";
    js::Value fn(c, JS_Eval(c, kFetchJs, sizeof kFetchJs - 1, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) {
        js::Value ex(c, JS_GetException(c));
        js::Value msg = ex.get("message");
        size_t len = 0;
        const char* s = JS_ToCStringLen(c, &len, msg.raw());
        std::string t = s ? std::string(s, len) : "?";
        if (s) JS_FreeCString(c, s);
        throw std::runtime_error(std::string(name) + ": " + t);
    }
    js::Value fetch = p.cx.call(fn, {natives, decoder});
    if (auto e = p.cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message);
    JS_SetPropertyStr(c, p.zk.raw(), "fetch", JS_DupValue(c, fetch.raw()));
}

}  // namespace zk::plugins
