[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / search

# Function: search()

```ts
function search(text): Promise<Item[]>;
```

Finds items by words, like the app's search box: matches title, creators, year, publication
and tags; best matches first. The one way to search; [items](items.md) lists by collection or tag.

**Permission**: `library:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `text` | `string` |

## Returns

`Promise`\<[`Item`](../Item.md)[]\>

## Example

```ts
const [best] = await zk.library.search("gaussian random fields adler");
```
