[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / createItem

# Function: createItem()

```ts
function createItem(fields, opts?): Promise<Change>;
```

Creates an item with the given metadata, optionally filed into collections and tagged.
One change: cancelling it withdraws the item with its collections and tags. The new item
is readable at once (`zk.library.item(change.key)`).

**Permission**: `zotero:create-items`; also `zotero:collections` if `collections` isn't
empty, `zotero:tags` if `tags` isn't empty.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `fields` | [`ItemFields`](../ItemFields.md) |
| `opts?` | \{ `collections?`: `string`[]; `tags?`: `string`[]; \} |
| `opts.collections?` | `string`[] |
| `opts.tags?` | `string`[] |

## Returns

`Promise`\<[`Change`](../Change.md)\>

## Example

```ts
const change = await zk.zotero.createItem(
  {
    type: "preprint",
    title: entry.title,
    creators: entry.authors.map((name) => ({ type: "author", name })),
    date: entry.date,
    arxiv: entry.id,
    url: `https://arxiv.org/abs/${entry.id}`,
    abstract: entry.summary,
  },
  { collections: [inbox.key], tags: ["feed-riffle"] },
);
await zk.storage.set(`undo:${entry.id}`, change.changeId);
```
