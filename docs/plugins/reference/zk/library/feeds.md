[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / feeds

# Function: feeds()

```ts
function feeds(): Promise<Feed[]>;
```

The user's Zotero feed subscriptions, as of the last sync, sorted by name. Read-only: the
app never changes them. Needs app 0.6.0.

**Permission**: `library:read`.

## Returns

`Promise`\<[`Feed`](../Feed.md)[]\>

## Example

```ts
const arxiv = (await zk.library.feeds()).filter((f) => f.url.startsWith("https://rss.arxiv.org/"));
```
