[Plugin API v0](../README.md) / [zk](../zk.md) / RequestInit

# Interface: RequestInit

Options for [zk.fetch](fetch.md).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="body"></a> `body?` | `string` | Request body (sent as UTF-8, at most 10 MB; not with GET or HEAD); send JSON with `JSON.stringify` and a `content-type` header. |
| <a id="headers"></a> `headers?` | `Record`\<`string`, `string`\> \| \[`string`, `string`\][] | Request headers, as an object or as `[name, value]` pairs (for repeated names). `Cookie` is not allowed. |
| <a id="method"></a> `method?` | `"GET"` \| `"HEAD"` \| `"POST"` \| `"PUT"` \| `"PATCH"` \| `"DELETE"` | Default `"GET"`. |
| <a id="timeout"></a> `timeout?` | `number` | Give up after this many milliseconds (at most, and by default, 30 000; a larger value counts as 30 000), rejecting with a [zk.LimitError](LimitError.md) `"responseTime"`. The time counts from the call, redirects included. For calls that share a budget, e.g. several feeds fetched in one 30-second background run. **Example** `const res = await zk.fetch(feedUrl, { timeout: 8000 });` |
