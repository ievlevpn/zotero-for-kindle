[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / items

# Function: items()

```ts
function items(query?): AsyncIterable<Item>;
```

Every item matching `query` (all items if none), in the library's order, fetched lazily —
so a whole library can be walked without loading it at once. Filters combine (AND). To find
items by words (title, creators…), use [search](search.md).

**Permission**: `library:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `query?` | [`ItemsQuery`](../ItemsQuery.md) |

## Returns

`AsyncIterable`\<[`Item`](../Item.md)\>

## Examples

```ts
for await (const item of zk.library.items({ collection: inbox.key })) {
  if (!item.read) console.log(item.title);
}
```

```ts
// first ten items tagged "to-read"
for await (const item of zk.library.items({ tag: "to-read", limit: 10 })) console.log(item.title);
```
