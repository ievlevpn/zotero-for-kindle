[Plugin API v0](../README.md) / [zk](../zk.md) / fetch

# Function: fetch()

```ts
function fetch(url, init?): Promise<Response>;
```

Fetches a URL, like the web's `fetch`, but HTTPS only, to declared hosts only, without cookies
and without any of the app's credentials. Responses are capped at 10 MB and requests at 30 s
each (less with [RequestInit.timeout](RequestInit.md#timeout)); going over rejects with a [zk.LimitError](LimitError.md)
(`"responseSize"`, `"responseTime"`). A plugin may have at most 8 fetches under way at once,
with at most 20 MB of request bodies between them; one more rejects at once with a
[zk.LimitError](LimitError.md) `"fetches"` (await some first).
`zotero.org`, every `*.zotero.org` host and the user's WebDAV host are always refused, even
with `net:*`. With `net:*`, hosts on the local network (loopback, private and link-local
addresses, checked on the address actually connected to) are refused too; declare such a host
by name (`net:<host>`) to reach it.

**Permission**: `net:<host>` for the URL's host (e.g. `"net:export.arxiv.org"`; `"net:*.example.org"`
covers subdomains), or `net:*` for any host. Redirects are checked the same way.

Network errors reject with an `Error`; HTTP errors don't reject: check `ok` or `status`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `url` | `string` |
| `init?` | [`RequestInit`](RequestInit.md) |

## Returns

`Promise`\<[`Response`](Response.md)\>

## Examples

```ts
const res = await zk.fetch("https://export.arxiv.org/rss/math.PR");
if (!res.ok) throw new Error(`arXiv: HTTP ${res.status}`);
const xml = await res.text();
```

```ts
const res = await zk.fetch("https://api.example.org/v1/explain", {
  method: "POST",
  headers: { "content-type": "application/json", authorization: `Bearer ${key}` },
  body: JSON.stringify({ text }),
});
const { answer } = await res.json<{ answer: string }>();
```
