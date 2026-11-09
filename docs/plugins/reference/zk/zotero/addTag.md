[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / addTag

# Function: addTag()

```ts
function addTag(itemKey, tag): Promise<Change>;
```

Adds a tag to an item (nothing changes if it already has it). `change.key` is the item's key.

**Permission**: `zotero:tags`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |
| `tag` | `string` |

## Returns

`Promise`\<[`Change`](../Change.md)\>

## Example

```ts
await zk.zotero.addTag(item.key, "finished-on-kindle");
```
