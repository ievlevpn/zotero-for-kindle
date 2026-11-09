[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / addToCollection

# Function: addToCollection()

```ts
function addToCollection(itemKey, collectionKey): Promise<Change>;
```

Files an item into a collection (it stays in its other collections). `change.key` is the
item's key.

**Permission**: `zotero:collections`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |
| `collectionKey` | `string` |

## Returns

`Promise`\<[`Change`](../Change.md)\>

## Example

```ts
await zk.zotero.addToCollection(item.key, inbox.key);
```
