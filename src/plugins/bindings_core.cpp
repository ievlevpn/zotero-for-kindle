// The core of `zk` and the standard globals every plugin gets: zk.plugin, zk.settings, zk.on,
// zk.background, the error classes, console, setTimeout/clearTimeout, and the web's URL,
// URLSearchParams, TextEncoder and TextDecoder (QuickJS-ng has none of these four; a small
// WHATWG-faithful polyfill below covers what zk.d.ts declares, with UTF-8 done natively).

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "plugins/plugin_ctx.h"

namespace zk::plugins {

namespace {

// ---- UTF-8, natively: the WHATWG decoder (U+FFFD per maximal subpart) and encoder ----------

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Decodes `n` bytes into valid UTF-8 (`out`). `used`: bytes consumed — with `stream`, an
// incomplete sequence at the end is left for the next chunk. `bad`: an invalid sequence was seen.
void decodeUtf8(const uint8_t* p, size_t n, bool stream, std::string& out, size_t& used, bool& bad) {
    static const char kReplacement[] = "\xEF\xBF\xBD";
    uint32_t cp = 0;
    int needed = 0, seen = 0;
    uint8_t lower = 0x80, upper = 0xBF;
    size_t start = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = p[i];
        if (needed == 0) {
            start = i;
            if (b <= 0x7F) {
                out += static_cast<char>(b);
            } else if (b >= 0xC2 && b <= 0xDF) {
                needed = 1;
                cp = b & 0x1F;
            } else if (b >= 0xE0 && b <= 0xEF) {
                if (b == 0xE0) lower = 0xA0;
                if (b == 0xED) upper = 0x9F;
                needed = 2;
                cp = b & 0x0F;
            } else if (b >= 0xF0 && b <= 0xF4) {
                if (b == 0xF0) lower = 0x90;
                if (b == 0xF4) upper = 0x8F;
                needed = 3;
                cp = b & 0x07;
            } else {
                out += kReplacement;
                bad = true;
            }
            continue;
        }
        if (b < lower || b > upper) {  // the sequence ends early: U+FFFD, and this byte starts afresh
            cp = 0;
            needed = seen = 0;
            lower = 0x80;
            upper = 0xBF;
            out += kReplacement;
            bad = true;
            --i;
            continue;
        }
        lower = 0x80;
        upper = 0xBF;
        cp = (cp << 6) | (b & 0x3F);
        if (++seen == needed) {
            appendUtf8(out, cp);
            cp = 0;
            needed = seen = 0;
        }
    }
    used = n;
    if (needed != 0) {
        if (stream) {
            used = start;
        } else {
            out += kReplacement;
            bad = true;
        }
    }
}

// utf8Encode(string) -> Uint8Array; lone surrogates become U+FFFD.
JSValue jsUtf8Encode(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    size_t len = 0;
    const char* s = JS_ToCStringLen2(c, &len, argc > 0 ? argv[0] : JS_UNDEFINED, false);
    if (!s) return JS_EXCEPTION;
    // QuickJS writes a lone surrogate as its 3-byte form (ED A0..BF xx); paired ones are joined.
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const auto b = static_cast<uint8_t>(s[i]);
        if (b == 0xED && i + 2 < len && static_cast<uint8_t>(s[i + 1]) >= 0xA0) {
            out += "\xEF\xBF\xBD";
            i += 2;
        } else {
            out += static_cast<char>(b);
        }
    }
    JS_FreeCString(c, s);
    return JS_NewUint8ArrayCopy(c, reinterpret_cast<const uint8_t*>(out.data()), out.size());
}

// utf8Decode(Uint8Array, fatal, stream) -> [string, bytes consumed]
JSValue jsUtf8Decode(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    size_t n = 0;
    const uint8_t* data = argc > 0 ? JS_GetUint8Array(c, &n, argv[0]) : nullptr;
    if (!data && JS_HasException(c)) return JS_EXCEPTION;
    const bool fatal = argc > 1 && JS_ToBool(c, argv[1]) > 0;
    const bool stream = argc > 2 && JS_ToBool(c, argv[2]) > 0;
    std::string out;
    size_t used = 0;
    bool bad = false;
    if (data) decodeUtf8(data, n, stream, out, used, bad);
    if (bad && fatal) return JS_ThrowTypeError(c, "TextDecoder: the data is not valid UTF-8");
    JSValue arr = JS_NewArray(c);
    if (JS_IsException(arr)) return arr;
    JS_SetPropertyUint32(c, arr, 0, JS_NewStringLen(c, out.data(), out.size()));
    JS_SetPropertyUint32(c, arr, 1, JS_NewInt64(c, static_cast<int64_t>(used)));
    return arr;
}

// ---- The standard globals, in JavaScript ------------------------------------------------
// Evaluated as a script that yields a function of the natives; nothing but the globals leaks.

const char kStandardGlobals[] = R"JS((function (native) {
"use strict";
const { utf8Encode, utf8Decode } = native;
const define = (name, value) =>
  Object.defineProperty(globalThis, name, { value, writable: true, configurable: true });

// ---- TextEncoder / TextDecoder (UTF-8 only) ----

class TextEncoder {
  get encoding() { return "utf-8"; }
  encode(input = "") { return utf8Encode(String(input)); }
}

const UTF8_LABELS = ["unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "utf-8", "utf8", "x-unicode20utf8"];

class TextDecoder {
  #fatal;
  #ignoreBOM;
  #pending = null;  // the start of a sequence split across stream chunks
  #started = false; // a code point was output (so a BOM now is content)
  constructor(label = "utf-8", options = undefined) {
    const l = String(label).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "").toLowerCase();
    if (!UTF8_LABELS.includes(l)) throw new RangeError(`TextDecoder: only UTF-8 is supported, not "${label}"`);
    this.#fatal = Boolean(options?.fatal);
    this.#ignoreBOM = Boolean(options?.ignoreBOM);
  }
  get encoding() { return "utf-8"; }
  get fatal() { return this.#fatal; }
  get ignoreBOM() { return this.#ignoreBOM; }
  decode(input = undefined, options = undefined) {
    let bytes;
    if (input === undefined) bytes = new Uint8Array(0);
    else if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
    else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    else throw new TypeError("TextDecoder.decode: expected an ArrayBuffer, a typed array or a DataView");
    const stream = Boolean(options?.stream);
    if (this.#pending) {
      const joined = new Uint8Array(this.#pending.length + bytes.length);
      joined.set(this.#pending);
      joined.set(bytes, this.#pending.length);
      bytes = joined;
      this.#pending = null;
    }
    let text, used;
    try {
      [text, used] = utf8Decode(bytes, this.#fatal, stream);
    } catch (e) {
      this.#started = false;
      throw e;
    }
    if (used < bytes.length) this.#pending = bytes.slice(used);
    if (!this.#ignoreBOM && !this.#started && text.length > 0) {
      if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);
      this.#started = true;
    }
    if (!stream) {
      this.#started = false;
      this.#pending = null;
    }
    return text;
  }
}

// ---- Percent-encoding (WHATWG URL §1.3) ----

const HEX = "0123456789ABCDEF";
const pct = (b) => "%" + HEX[b >> 4] + HEX[b & 15];
const C0 = (c) => c < 0x20 || c > 0x7e;
const FRAGMENT = (c) => C0(c) || c === 0x20 || c === 0x22 || c === 0x3c || c === 0x3e || c === 0x60;
const QUERY = (c) => C0(c) || c === 0x20 || c === 0x22 || c === 0x23 || c === 0x3c || c === 0x3e;
const SPECIAL_QUERY = (c) => QUERY(c) || c === 0x27;
const PATH = (c) => QUERY(c) || c === 0x3f || c === 0x5e || c === 0x60 || c === 0x7b || c === 0x7d;
const USERINFO = (c) =>
  PATH(c) || c === 0x2f || c === 0x3a || c === 0x3b || c === 0x3d || c === 0x40 || (c >= 0x5b && c <= 0x5d) || c === 0x7c;
// application/x-www-form-urlencoded: only ASCII alphanumerics and *-._ stay as they are
const FORM = (c) => !((c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) ||
  c === 0x2a || c === 0x2d || c === 0x2e || c === 0x5f);

function encode(str, set, spaceAsPlus = false) {
  let out = "";
  for (const ch of str) {
    const c = ch.codePointAt(0);
    if (c < 0x80) out += spaceAsPlus && c === 0x20 ? "+" : set(c) ? pct(c) : ch;
    else for (const b of utf8Encode(ch)) out += pct(b);
  }
  return out;
}

const hexValue = (b) => (b >= 0x30 && b <= 0x39 ? b - 0x30 : b >= 0x41 && b <= 0x46 ? b - 0x37 : b >= 0x61 && b <= 0x66 ? b - 0x57 : -1);

function percentDecode(str) {
  const src = utf8Encode(str);
  const out = new Uint8Array(src.length);
  let n = 0;
  for (let i = 0; i < src.length; i++) {
    if (src[i] === 0x25 && i + 2 < src.length) {
      const h = hexValue(src[i + 1]), l = hexValue(src[i + 2]);
      if (h >= 0 && l >= 0) {
        out[n++] = h * 16 + l;
        i += 2;
        continue;
      }
    }
    out[n++] = src[i];
  }
  return utf8Decode(out.subarray(0, n), false, false)[0];
}

function parseForm(input) {
  const list = [];
  for (const seq of input.split("&")) {
    if (seq === "") continue;
    const eq = seq.indexOf("=");
    const name = eq < 0 ? seq : seq.slice(0, eq);
    const value = eq < 0 ? "" : seq.slice(eq + 1);
    list.push([percentDecode(name.replace(/\+/g, " ")), percentDecode(value.replace(/\+/g, " "))]);
  }
  return list;
}

const serializeForm = (list) => list.map(([k, v]) => encode(k, FORM, true) + "=" + encode(v, FORM, true)).join("&");

// ---- Hosts ----

const FORBIDDEN_HOST = new Set([0x00, 0x09, 0x0a, 0x0d, 0x20, 0x23, 0x2f, 0x3a, 0x3c, 0x3e, 0x3f, 0x40, 0x5b, 0x5c, 0x5d, 0x5e, 0x7c]);
const forbiddenDomain = (c) => FORBIDDEN_HOST.has(c) || c <= 0x1f || c === 0x25 || c === 0x7f;

function punycode(label) {
  const base = 36, tMin = 1, tMax = 26, skew = 38, damp = 700;
  const cps = Array.from(label, (ch) => ch.codePointAt(0));
  let n = 128, delta = 0, bias = 72, out = "";
  for (const c of cps) if (c < 0x80) out += String.fromCharCode(c);
  const b = out.length;
  let h = b;
  if (b > 0) out += "-";
  const digit = (d) => String.fromCharCode(d < 26 ? d + 97 : d + 22);
  const adapt = (d, points, first) => {
    d = first ? Math.floor(d / damp) : d >> 1;
    d += Math.floor(d / points);
    let k = 0;
    while (d > ((base - tMin) * tMax) >> 1) {
      d = Math.floor(d / (base - tMin));
      k += base;
    }
    return k + Math.floor(((base - tMin + 1) * d) / (d + skew));
  };
  while (h < cps.length) {
    let m = Infinity;
    for (const c of cps) if (c >= n && c < m) m = c;
    delta += (m - n) * (h + 1);
    n = m;
    for (const c of cps) {
      if (c < n) delta++;
      if (c === n) {
        let q = delta;
        for (let k = base; ; k += base) {
          const t = k <= bias ? tMin : k >= bias + tMax ? tMax : k - bias;
          if (q < t) break;
          out += digit(t + ((q - t) % (base - t)));
          q = Math.floor((q - t) / (base - t));
        }
        out += digit(q);
        bias = adapt(delta, h + 1, h === b);
        delta = 0;
        h++;
      }
    }
    delta++;
    n++;
  }
  return out;
}

// UTS #46 in brief: case-folded, NFKC, ideographic full stops as dots, non-ASCII labels as "xn--".
function domainToASCII(domain) {
  const d = domain.toLowerCase().normalize("NFKC").replace(/[。．｡]/g, ".");
  if (/^[\x00-\x7f]*$/.test(d)) return d;
  return d.split(".").map((l) => (/^[\x00-\x7f]*$/.test(l) ? l : "xn--" + punycode(l))).join(".");
}

function parseIPv4Number(s) {
  if (s === "") return NaN;
  let radix = 10;
  if (s.length >= 2 && s[0] === "0" && (s[1] === "x" || s[1] === "X")) {
    s = s.slice(2);
    radix = 16;
  } else if (s.length >= 2 && s[0] === "0") {
    s = s.slice(1);
    radix = 8;
  }
  if (s === "") return 0;
  const re = radix === 16 ? /^[0-9a-fA-F]+$/ : radix === 8 ? /^[0-7]+$/ : /^[0-9]+$/;
  return re.test(s) ? parseInt(s, radix) : NaN;
}

function endsInNumber(host) {
  const parts = host.split(".");
  if (parts[parts.length - 1] === "") {
    if (parts.length === 1) return false;
    parts.pop();
  }
  const last = parts[parts.length - 1];
  if (last !== "" && /^[0-9]+$/.test(last)) return true;
  return !Number.isNaN(parseIPv4Number(last));
}

function parseIPv4(host) {
  const parts = host.split(".");
  if (parts[parts.length - 1] === "" && parts.length > 1) parts.pop();
  if (parts.length > 4) return null;
  const nums = [];
  for (const p of parts) {
    const v = parseIPv4Number(p);
    if (Number.isNaN(v)) return null;
    nums.push(v);
  }
  for (let i = 0; i < nums.length - 1; i++) if (nums[i] > 255) return null;
  if (nums[nums.length - 1] >= 256 ** (5 - nums.length)) return null;
  let ipv4 = nums[nums.length - 1];
  for (let i = 0; i < nums.length - 1; i++) ipv4 += nums[i] * 256 ** (3 - i);
  return [24, 16, 8, 0].map((s) => Math.floor(ipv4 / 2 ** s) % 256).join(".");
}

function parseIPv6(s) {
  const address = [0, 0, 0, 0, 0, 0, 0, 0];
  let piece = 0, compress = null, p = 0;
  const isHex = (ch) => ch !== undefined && /[0-9a-fA-F]/.test(ch);
  const isDigit = (ch) => ch !== undefined && ch >= "0" && ch <= "9";
  if (s[p] === ":") {
    if (s[p + 1] !== ":") return null;
    p += 2;
    piece++;
    compress = piece;
  }
  while (p < s.length) {
    if (piece === 8) return null;
    if (s[p] === ":") {
      if (compress !== null) return null;
      p++;
      piece++;
      compress = piece;
      continue;
    }
    let value = 0, length = 0;
    while (length < 4 && isHex(s[p])) {
      value = value * 16 + parseInt(s[p], 16);
      p++;
      length++;
    }
    if (s[p] === ".") {
      if (length === 0) return null;
      p -= length;
      if (piece > 6) return null;
      let numbersSeen = 0;
      while (p < s.length) {
        let v4 = null;
        if (numbersSeen > 0) {
          if (s[p] === "." && numbersSeen < 4) p++;
          else return null;
        }
        if (!isDigit(s[p])) return null;
        while (isDigit(s[p])) {
          const n = s.charCodeAt(p) - 48;
          if (v4 === null) v4 = n;
          else if (v4 === 0) return null;
          else v4 = v4 * 10 + n;
          if (v4 > 255) return null;
          p++;
        }
        address[piece] = address[piece] * 0x100 + v4;
        numbersSeen++;
        if (numbersSeen === 2 || numbersSeen === 4) piece++;
      }
      if (numbersSeen !== 4) return null;
      break;
    } else if (s[p] === ":") {
      p++;
      if (p >= s.length) return null;
    } else if (p < s.length) {
      return null;
    }
    address[piece] = value;
    piece++;
  }
  if (compress !== null) {
    let swaps = piece - compress;
    piece = 7;
    while (piece !== 0 && swaps > 0) {
      const t = address[compress + swaps - 1];
      address[compress + swaps - 1] = address[piece];
      address[piece] = t;
      piece--;
      swaps--;
    }
  } else if (piece !== 8) {
    return null;
  }
  return address;
}

function serializeIPv6(a) {
  let best = -1, bestLength = 1;
  for (let i = 0; i < 8; ) {
    if (a[i] !== 0) { i++; continue; }
    let j = i;
    while (j < 8 && a[j] === 0) j++;
    if (j - i > bestLength) { best = i; bestLength = j - i; }
    i = j;
  }
  let out = "";
  for (let i = 0; i < 8; i++) {
    if (i === best) {
      out += i === 0 ? "::" : ":";
      i += bestLength - 1;
      continue;
    }
    out += a[i].toString(16);
    if (i !== 7) out += ":";
  }
  return out;
}

function parseHost(input, special) {
  if (input.startsWith("[")) {
    if (!input.endsWith("]")) return null;
    const a = parseIPv6(input.slice(1, -1));
    return a === null ? null : "[" + serializeIPv6(a) + "]";
  }
  if (!special) {
    for (let i = 0; i < input.length; i++) if (FORBIDDEN_HOST.has(input.charCodeAt(i))) return null;
    return encode(input, C0);
  }
  const ascii = domainToASCII(percentDecode(input));
  if (ascii === "") return null;
  for (let i = 0; i < ascii.length; i++) if (forbiddenDomain(ascii.charCodeAt(i))) return null;
  return endsInNumber(ascii) ? parseIPv4(ascii) : ascii;
}

// ---- URL records: parsing and serialising (WHATWG URL §4) ----

const SPECIAL = new Map([["ftp", 21], ["file", null], ["http", 80], ["https", 443], ["ws", 80], ["wss", 443]]);
const isSpecial = (scheme) => SPECIAL.has(scheme);
const defaultPort = (scheme) => SPECIAL.get(scheme) ?? null;
const isSlash = (ch, special) => ch === "/" || (special && ch === "\\");
const SCHEME = /^([A-Za-z][A-Za-z0-9+\-.]*):/;

const blank = (scheme) => ({ scheme, username: "", password: "", host: null, port: null, path: [], opaque: false, query: null, fragment: null });

function takeAuthority(url, base) {
  url.username = base.username;
  url.password = base.password;
  url.host = base.host;
  url.port = base.port;
}

function addPath(url, text) {
  const special = isSpecial(url.scheme);
  if (text === "") {
    if (special) url.path.push("");
    return;
  }
  const sep = special ? /[\/\\]/ : /\//;
  const segments = text.split(sep);
  if (sep.test(text[0])) segments.shift();
  segments.forEach((segment, i) => {
    const last = i === segments.length - 1;
    const s = segment.toLowerCase();
    if (s === ".." || s === ".%2e" || s === "%2e." || s === "%2e%2e") {
      url.path.pop();
      if (last) url.path.push("");
    } else if (s === "." || s === "%2e") {
      if (last) url.path.push("");
    } else {
      url.path.push(encode(segment, PATH));
    }
  });
}

function queryAndFragment(url, rest) {
  if (rest.startsWith("?")) {
    const h = rest.indexOf("#");
    url.query = encode(h < 0 ? rest.slice(1) : rest.slice(1, h), isSpecial(url.scheme) ? SPECIAL_QUERY : QUERY);
    rest = h < 0 ? "" : rest.slice(h);
  }
  if (rest.startsWith("#")) url.fragment = encode(rest.slice(1), FRAGMENT);
  return url;
}

function pathAndAfter(url, rest) {
  const q = rest.search(/[?#]/);
  addPath(url, q < 0 ? rest : rest.slice(0, q));
  return queryAndFragment(url, q < 0 ? "" : rest.slice(q));
}

// Splits "host[:port]" at the first colon outside brackets.
function splitHostPort(text) {
  let inBrackets = false;
  for (let i = 0; i < text.length; i++) {
    if (text[i] === "[") inBrackets = true;
    else if (text[i] === "]") inBrackets = false;
    else if (text[i] === ":" && !inBrackets) return [text.slice(0, i), text.slice(i + 1)];
  }
  return [text, undefined];
}

function authority(url, rest) {
  const special = isSpecial(url.scheme);
  let end = 0;
  while (end < rest.length && !"/?#".includes(rest[end]) && !(special && rest[end] === "\\")) end++;
  let auth = rest.slice(0, end);
  rest = rest.slice(end);
  const at = auth.lastIndexOf("@");
  if (at >= 0) {
    const info = auth.slice(0, at);
    auth = auth.slice(at + 1);
    const colon = info.indexOf(":");
    url.username = encode(colon < 0 ? info : info.slice(0, colon), USERINFO);
    url.password = colon < 0 ? "" : encode(info.slice(colon + 1), USERINFO);
    if (auth === "") return null;
  }
  const [hostText, portText] = splitHostPort(auth);
  if (hostText === "" && (special || portText !== undefined)) return null;
  if (portText !== undefined && portText !== "") {
    if (!/^[0-9]+$/.test(portText)) return null;
    const port = Number(portText);
    if (port > 65535) return null;
    url.port = port === defaultPort(url.scheme) ? null : port;
  }
  const host = parseHost(hostText, special);
  if (host === null) return null;
  url.host = host;
  return pathAndAfter(url, rest);
}

function relative(url, rest, base) {
  const special = isSpecial(url.scheme);
  if (isSlash(rest[0], special)) {
    if (isSlash(rest[1], special)) return authority(url, special ? rest.replace(/^[\/\\]*/, "") : rest.slice(2));
    takeAuthority(url, base);
    return pathAndAfter(url, rest);
  }
  takeAuthority(url, base);
  if (rest === "" || rest[0] === "?" || rest[0] === "#") {
    url.path = base.path.slice();
    if (rest === "" || rest[0] === "#") url.query = base.query;
    return queryAndFragment(url, rest);
  }
  url.path = base.path.slice(0, -1);
  return pathAndAfter(url, rest);
}

function file(url, rest, base) {
  url.host = "";
  if (isSlash(rest[0], true)) {
    if (isSlash(rest[1], true)) {
      rest = rest.slice(2);
      const end = rest.search(/[\/\\?#]/);
      const hostText = end < 0 ? rest : rest.slice(0, end);
      rest = end < 0 ? "" : rest.slice(end);
      if (hostText !== "") {
        const host = parseHost(hostText, true);
        if (host === null) return null;
        url.host = host === "localhost" ? "" : host;
      }
      return pathAndAfter(url, rest);
    }
    if (base) url.host = base.host;
    return pathAndAfter(url, rest);
  }
  if (base) {
    url.host = base.host;
    if (rest === "" || rest[0] === "?" || rest[0] === "#") {
      url.path = base.path.slice();
      if (rest === "" || rest[0] === "#") url.query = base.query;
      return queryAndFragment(url, rest);
    }
    url.path = base.path.slice(0, -1);
  }
  return pathAndAfter(url, rest);
}

function parseURL(input, base) {
  input = input.replace(/^[\u0000- ]+|[\u0000- ]+$/g, "").replace(/[\t\n\r]/g, "");
  const m = SCHEME.exec(input);
  if (m) {
    const url = blank(m[1].toLowerCase());
    const rest = input.slice(m[0].length);
    if (url.scheme === "file") return file(url, rest, base && base.scheme === "file" ? base : null);
    if (isSpecial(url.scheme)) {
      if (base && base.scheme === url.scheme && !rest.startsWith("//")) return relative(url, rest, base);
      return authority(url, rest.replace(/^[\/\\]*/, ""));
    }
    if (rest.startsWith("//")) return authority(url, rest.slice(2));
    if (rest.startsWith("/")) return pathAndAfter(url, rest);
    const q = rest.search(/[?#]/);
    url.opaque = true;
    url.path = encode(q < 0 ? rest : rest.slice(0, q), C0);
    return queryAndFragment(url, q < 0 ? "" : rest.slice(q));
  }
  if (!base) return null;
  if (base.opaque) {
    if (!input.startsWith("#")) return null;
    const url = { ...base };
    url.fragment = encode(input.slice(1), FRAGMENT);
    return url;
  }
  const url = blank(base.scheme);
  return base.scheme === "file" ? file(url, input, base) : relative(url, input, base);
}

function serialize(u) {
  let out = u.scheme + ":";
  if (u.host !== null) {
    out += "//";
    if (u.username !== "" || u.password !== "") {
      out += u.username;
      if (u.password !== "") out += ":" + u.password;
      out += "@";
    }
    out += u.host;
    if (u.port !== null) out += ":" + u.port;
  }
  if (u.opaque) {
    out += u.path;
  } else {
    if (u.host === null && u.path.length > 1 && u.path[0] === "") out += "/.";
    for (const s of u.path) out += "/" + s;
  }
  if (u.query !== null) out += "?" + u.query;
  if (u.fragment !== null) out += "#" + u.fragment;
  return out;
}

const stripControls = (v) => String(v).replace(/[\t\n\r]/g, "");

// ---- URLSearchParams ----

let setParamsList, setParamsListener;

class URLSearchParams {
  #list = [];
  #listener = null;  // the URL these params belong to: told of each change
  static {
    setParamsList = (params, list) => { params.#list = list; };
    setParamsListener = (params, f) => { params.#listener = f; };
  }
  constructor(init = undefined) {
    if (init === undefined || init === null) return;
    if (typeof init === "object" || typeof init === "function") {
      if (typeof init[Symbol.iterator] === "function") {
        for (const pair of init) {
          const p = Array.from(pair);
          if (p.length !== 2) throw new TypeError("URLSearchParams: each pair must be [name, value]");
          this.#list.push([String(p[0]), String(p[1])]);
        }
      } else {
        for (const k of Object.keys(init)) this.#list.push([k, String(init[k])]);
      }
    } else {
      const s = String(init);
      this.#list = parseForm(s.startsWith("?") ? s.slice(1) : s);
    }
  }
  #changed() {
    if (this.#listener) this.#listener(serializeForm(this.#list));
  }
  get size() { return this.#list.length; }
  append(name, value) {
    this.#list.push([String(name), String(value)]);
    this.#changed();
  }
  delete(name, value = undefined) {
    name = String(name);
    const v = value === undefined ? undefined : String(value);
    this.#list = this.#list.filter(([k, x]) => !(k === name && (v === undefined || x === v)));
    this.#changed();
  }
  get(name) {
    name = String(name);
    const e = this.#list.find(([k]) => k === name);
    return e ? e[1] : null;
  }
  getAll(name) {
    name = String(name);
    return this.#list.filter(([k]) => k === name).map(([, v]) => v);
  }
  has(name, value = undefined) {
    name = String(name);
    const v = value === undefined ? undefined : String(value);
    return this.#list.some(([k, x]) => k === name && (v === undefined || x === v));
  }
  set(name, value) {
    name = String(name);
    value = String(value);
    const i = this.#list.findIndex(([k]) => k === name);
    if (i < 0) {
      this.#list.push([name, value]);
    } else {
      this.#list[i] = [name, value];
      this.#list = this.#list.filter(([k], j) => j <= i || k !== name);
    }
    this.#changed();
  }
  sort() {
    this.#list.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));  // stable, by code units
    this.#changed();
  }
  forEach(callback, thisArg = undefined) {
    for (let i = 0; i < this.#list.length; i++) callback.call(thisArg, this.#list[i][1], this.#list[i][0], this);
  }
  *keys() { for (let i = 0; i < this.#list.length; i++) yield this.#list[i][0]; }
  *values() { for (let i = 0; i < this.#list.length; i++) yield this.#list[i][1]; }
  *entries() { for (let i = 0; i < this.#list.length; i++) yield [this.#list[i][0], this.#list[i][1]]; }
  [Symbol.iterator]() { return this.entries(); }
  toString() { return serializeForm(this.#list); }
}

// ---- URL ----

class URL {
  #r;
  #params;
  constructor(url, base = undefined) {
    let b = null;
    if (base !== undefined) {
      b = parseURL(String(base), null);
      if (b === null) throw new TypeError(`Invalid base URL: ${base}`);
    }
    const r = parseURL(String(url), b);
    if (r === null) throw new TypeError(`Invalid URL: ${url}`);
    this.#r = r;
    this.#params = new URLSearchParams(r.query ?? "");
    setParamsListener(this.#params, (q) => { this.#r.query = q === "" ? null : q; });
  }
  static canParse(url, base = undefined) {
    try {
      new URL(url, base);
      return true;
    } catch {
      return false;
    }
  }
  get href() { return serialize(this.#r); }
  set href(v) {
    const r = parseURL(String(v), null);
    if (r === null) throw new TypeError(`Invalid URL: ${v}`);
    this.#r = r;
    setParamsList(this.#params, parseForm(r.query ?? ""));
  }
  get origin() {
    const r = this.#r;
    if (!isSpecial(r.scheme) || r.scheme === "file") return "null";
    return r.scheme + "://" + r.host + (r.port !== null ? ":" + r.port : "");
  }
  get protocol() { return this.#r.scheme + ":"; }
  set protocol(v) {
    const m = SCHEME.exec(stripControls(v) + ":");
    if (!m) return;
    const r = this.#r, scheme = m[1].toLowerCase();
    if (isSpecial(r.scheme) !== isSpecial(scheme)) return;
    if (scheme === "file" && (r.username !== "" || r.password !== "" || r.port !== null)) return;
    if (r.scheme === "file" && r.host === "") return;
    r.scheme = scheme;
    if (r.port !== null && r.port === defaultPort(scheme)) r.port = null;
  }
  get username() { return this.#r.username; }
  set username(v) {
    const r = this.#r;
    if (r.host === null || r.host === "" || r.scheme === "file") return;
    r.username = encode(stripControls(v), USERINFO);
  }
  get password() { return this.#r.password; }
  set password(v) {
    const r = this.#r;
    if (r.host === null || r.host === "" || r.scheme === "file") return;
    r.password = encode(stripControls(v), USERINFO);
  }
  get host() {
    const r = this.#r;
    if (r.host === null) return "";
    return r.port === null ? r.host : r.host + ":" + r.port;
  }
  set host(v) { this.#setHost(stripControls(v), true); }
  get hostname() { return this.#r.host ?? ""; }
  set hostname(v) { this.#setHost(stripControls(v), false); }
  #setHost(v, withPort) {
    const r = this.#r;
    if (r.opaque) return;
    const special = isSpecial(r.scheme);
    const end = v.search(special ? /[\/?#\\]/ : /[\/?#]/);
    if (end >= 0) v = v.slice(0, end);
    const [hostText, portText] = splitHostPort(v);
    if (hostText === "" && (special || r.username !== "" || r.password !== "" || r.port !== null)) return;
    const host = parseHost(hostText, special);
    if (host === null) return;
    r.host = host;
    if (withPort && portText !== undefined) {
      const digits = /^[0-9]*/.exec(portText)[0];
      if (digits !== "" && Number(digits) <= 65535) {
        const port = Number(digits);
        r.port = port === defaultPort(r.scheme) ? null : port;
      }
    }
  }
  get port() { return this.#r.port === null ? "" : String(this.#r.port); }
  set port(v) {
    const r = this.#r;
    if (r.host === null || r.host === "" || r.scheme === "file") return;
    v = stripControls(v);
    if (v === "") {
      r.port = null;
      return;
    }
    const digits = /^[0-9]*/.exec(v)[0];
    if (digits === "" || Number(digits) > 65535) return;
    const port = Number(digits);
    r.port = port === defaultPort(r.scheme) ? null : port;
  }
  get pathname() {
    const r = this.#r;
    if (r.opaque) return r.path;
    return r.path.length ? "/" + r.path.join("/") : "";
  }
  set pathname(v) {
    const r = this.#r;
    if (r.opaque) return;
    r.path = [];
    addPath(r, stripControls(v));
  }
  get search() {
    const q = this.#r.query;
    return q === null || q === "" ? "" : "?" + q;
  }
  set search(v) {
    const r = this.#r;
    v = stripControls(v);
    if (v === "") {
      r.query = null;
      setParamsList(this.#params, []);
      return;
    }
    if (v[0] === "?") v = v.slice(1);
    r.query = encode(v, isSpecial(r.scheme) ? SPECIAL_QUERY : QUERY);
    setParamsList(this.#params, parseForm(v));
  }
  get searchParams() { return this.#params; }
  get hash() {
    const f = this.#r.fragment;
    return f === null || f === "" ? "" : "#" + f;
  }
  set hash(v) {
    v = stripControls(v);
    if (v === "") {
      this.#r.fragment = null;
      return;
    }
    this.#r.fragment = encode(v[0] === "#" ? v.slice(1) : v, FRAGMENT);
  }
  toString() { return this.href; }
  toJSON() { return this.href; }
}

define("TextEncoder", TextEncoder);
define("TextDecoder", TextDecoder);
define("URLSearchParams", URLSearchParams);
define("URL", URL);
}))JS";

// ---- zk's own core, in JavaScript: errors, console, zk.plugin, zk.settings ---------------

const char kCore[] = R"JS((function (native, zk, info) {
"use strict";
const token = Symbol("zk");

class PermissionError extends Error {
  constructor(t, message, permission) {
    if (t !== token) throw new TypeError("zk.PermissionError is made by the app; a plugin can't construct one");
    super(message);
    Object.defineProperty(this, "permission", { value: permission, enumerable: true });
  }
}
class LimitError extends Error {
  constructor(t, limit, message) {
    if (t !== token) throw new TypeError("zk.LimitError is made by the app; a plugin can't construct one");
    super(message);
    Object.defineProperty(this, "limit", { value: limit, enumerable: true });
  }
}
for (const C of [PermissionError, LimitError])
  Object.defineProperty(C.prototype, "name", { value: C.name, writable: true, configurable: true });

const show = (x) => {
  if (typeof x === "string") return x;
  if (x instanceof Error) {
    const head = `${x.name}: ${x.message}`;
    return typeof x.stack === "string" && x.stack.trim() !== "" ? `${head}\n${x.stack.trimEnd()}` : head;
  }
  if (typeof x === "object" && x !== null) {
    try {
      const json = JSON.stringify(x);
      if (json !== undefined) return json;
    } catch {}
  }
  try { return String(x); } catch { return Object.prototype.toString.call(x); }
};
const writer = (level) => (...data) => native.log(level, data.map(show).join(" "));
const console = { log: writer(0), info: writer(0), debug: writer(0), warn: writer(1), error: writer(2) };
Object.defineProperty(globalThis, "console", { value: Object.freeze(console), writable: true, configurable: true });

const freeze = (o) => {
  if (o !== null && typeof o === "object") {
    for (const v of Object.values(o)) freeze(v);
    Object.freeze(o);
  }
  return o;
};
zk.plugin = Object.freeze({ id: info.id, version: info.version, dataDir: info.dataDir });
zk.settings = freeze(JSON.parse(info.settings));
zk.PermissionError = PermissionError;
zk.LimitError = LimitError;

return {
  permissionError: (message, permission) => new PermissionError(token, message, permission),
  limitError: (limit, message) => new LimitError(token, limit, message),
};
}))JS";

std::string str(JSContext* c, JSValueConst v) {
    size_t len = 0;
    const char* s = JS_ToCStringLen(c, &len, v);
    if (!s) return {};
    std::string out(s, len);
    JS_FreeCString(c, s);
    return out;
}

// Evaluates one of the scripts above and calls the function it yields with `args`.
js::Value runPrelude(js::Context& cx, const char* source, size_t size, const char* name, std::vector<js::Value> args) {
    JSContext* c = cx.raw();
    js::Value fn(c, JS_Eval(c, source, size, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT));
    if (JS_IsException(fn.raw())) {
        js::Value ex(c, JS_GetException(c));
        js::Value stack = ex.get("stack");
        throw std::runtime_error(std::string(name) + ": " + str(c, ex.raw()) + "\n" + str(c, stack.raw()));
    }
    js::Value r = cx.call(fn, std::move(args));
    if (auto e = cx.takeError()) throw std::runtime_error(std::string(name) + ": " + e->message + "\n" + e->stack);
    return r;
}

// ---- Natives ------------------------------------------------------------------------------

JSValue jsLog(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    int32_t level = 0;
    if (argc > 0) JS_ToInt32(c, &level, argv[0]);
    log(ctxOf(c), level == 2 ? LogLevel::Error : level == 1 ? LogLevel::Warn : LogLevel::Info,
        argc > 1 ? str(c, argv[1]) : std::string());
    return JS_UNDEFINED;
}

JSValue jsSetTimeout(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsFunction(c, argv[0])) return JS_ThrowTypeError(c, "setTimeout: the callback must be a function");
    double ms = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToFloat64(c, &ms, argv[1])) return JS_EXCEPTION;
    if (!(ms >= 0)) ms = 0;  // also NaN
    if (ms > 2147483647.0) ms = 2147483647.0;
    uint64_t id = addTimer(ctxOf(c), js::Value(c, JS_DupValue(c, argv[0])), static_cast<int64_t>(ms));
    return JS_NewInt64(c, static_cast<int64_t>(id));
}

JSValue jsClearTimeout(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    if (argc > 0 && JS_IsNumber(argv[0])) {
        int64_t id = 0;
        if (JS_ToInt64(c, &id, argv[0]) == 0 && id > 0) clearTimer(ctxOf(c), static_cast<uint64_t>(id));
    }
    return JS_UNDEFINED;
}

struct EventInfo {
    const char* name;
    std::optional<Permission> permission;
};
const EventInfo kEvents[] = {
    {"highlightCreated", Permission::AnnotationsRead}, {"highlightChanged", Permission::AnnotationsRead},
    {"highlightDeleted", Permission::AnnotationsRead}, {"bookOpened", Permission::LibraryRead},
    {"bookFinished", Permission::LibraryRead},         {"itemMarkedRead", Permission::LibraryRead},
    {"itemMarkedUnread", Permission::LibraryRead},     {"syncFinished", std::nullopt},
};

JSValue jsOn(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(c, "zk.on: the first argument is the event's name");
    const std::string name = str(c, argv[0]);
    const EventInfo* event = nullptr;
    std::string names;
    for (const auto& e : kEvents) {
        if (name == e.name) event = &e;
        names += (names.empty() ? "" : ", ") + std::string(e.name);
    }
    if (!event) return JS_ThrowTypeError(c, "zk.on: there is no event \"%s\" (events: %s)", name.c_str(), names.c_str());
    if (argc < 2 || !JS_IsFunction(c, argv[1])) return JS_ThrowTypeError(c, "zk.on(\"%s\"): the handler must be a function", name.c_str());
    if (event->permission && !requirePermission(p, *event->permission, "zk.on(\"" + name + "\")")) return JS_EXCEPTION;
    addHandler(p, name, js::Value(c, JS_DupValue(c, argv[1])));
    return JS_UNDEFINED;
}

JSValue jsBackground(JSContext* c, JSValueConst, int argc, JSValueConst* argv) {
    PluginCtx& p = ctxOf(c);
    if (!requirePermission(p, Permission::Background, "zk.background()")) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsFunction(c, argv[0])) return JS_ThrowTypeError(c, "zk.background: the task must be a function");
    setBackgroundTask(p, js::Value(c, JS_DupValue(c, argv[0])));
    return JS_UNDEFINED;
}

}  // namespace

// ---- Helpers for every binding --------------------------------------------------------------

void defineFunction(JSContext* c, JSValueConst obj, const char* name, JSCFunction* fn, int length) {
    JS_SetPropertyStr(c, obj, name, JS_NewCFunction(c, fn, name, length));
}

std::string permissionName(Permission p) {
    switch (p) {
        case Permission::LibraryRead: return "library:read";
        case Permission::AnnotationsRead: return "annotations:read";
        case Permission::NotesRead: return "notes:read";
        case Permission::FulltextRead: return "fulltext:read";
        case Permission::Net: return "net:";
        case Permission::NetAny: return "net:*";
        case Permission::CreateItems: return "zotero:create-items";
        case Permission::Collections: return "zotero:collections";
        case Permission::Tags: return "zotero:tags";
        case Permission::Notes: return "zotero:notes";
        case Permission::Remove: return "zotero:remove";
        case Permission::Background: return "background";
    }
    return "";
}

// Natives call the factories with JS_Call, not Context::call: that would record an exception as
// the entry's error (even one the plugin catches) and re-anchor the stack check deeper.
JSValue throwPermissionError(PluginCtx& p, const std::string& permission, const std::string& detail) {
    JSContext* c = p.cx.raw();
    const std::string message = detail + " needs \"" + permission + "\" in plugin.json";
    JSValue args[2] = {JS_NewStringLen(c, message.data(), message.size()), JS_NewStringLen(c, permission.data(), permission.size())};
    JSValue e = JS_Call(c, p.makePermissionError.raw(), JS_UNDEFINED, 2, args);
    JS_FreeValue(c, args[0]);
    JS_FreeValue(c, args[1]);
    if (JS_IsException(e)) return e;  // couldn't make one (out of memory): that exception instead
    return JS_Throw(c, e);
}

bool requirePermission(PluginCtx& p, Permission perm, const std::string& detail) {
    if (p.manifest.permissions.count(perm)) return true;
    throwPermissionError(p, permissionName(perm), detail);
    return false;
}

JSValue newLimitError(PluginCtx& p, const std::string& limit, const std::string& message) {
    JSContext* c = p.cx.raw();
    JSValue args[2] = {JS_NewStringLen(c, limit.data(), limit.size()), JS_NewStringLen(c, message.data(), message.size())};
    JSValue e = JS_Call(c, p.makeLimitError.raw(), JS_UNDEFINED, 2, args);
    JS_FreeValue(c, args[0]);
    JS_FreeValue(c, args[1]);
    return e;
}

// ---- Installers -----------------------------------------------------------------------------

bool installStandardGlobals(js::Context& cx) {
    JSContext* c = cx.raw();
    js::Value natives(c, JS_NewObject(c));
    defineFunction(c, natives.raw(), "utf8Encode", jsUtf8Encode, 1);
    defineFunction(c, natives.raw(), "utf8Decode", jsUtf8Decode, 3);
    try {
        runPrelude(cx, kStandardGlobals, sizeof kStandardGlobals - 1, "<standard globals>", {natives});
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void installCore(PluginCtx& p) {
    JSContext* c = p.cx.raw();
    if (!installStandardGlobals(p.cx)) throw std::runtime_error("the standard globals failed to install");

    js::Value natives(c, JS_NewObject(c));
    defineFunction(c, natives.raw(), "log", jsLog, 2);
    nlohmann::json info = {{"id", p.manifest.id}, {"version", p.manifest.version}, {"dataDir", p.dataDir},
                           {"settings", p.settings.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)}};
    js::Value made = runPrelude(p.cx, kCore, sizeof kCore - 1, "<zk core>", {natives, p.zk, js::Value::fromJson(p.cx, info)});
    p.makePermissionError = made.get("permissionError");
    p.makeLimitError = made.get("limitError");

    JSValue global = JS_GetGlobalObject(c);
    defineFunction(c, global, "setTimeout", jsSetTimeout, 2);
    defineFunction(c, global, "clearTimeout", jsClearTimeout, 1);
    JS_FreeValue(c, global);
    defineFunction(c, p.zk.raw(), "on", jsOn, 2);
    defineFunction(c, p.zk.raw(), "background", jsBackground, 1);
}

}  // namespace zk::plugins
