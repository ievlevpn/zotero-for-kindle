[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / removeFromCollection

# Function: removeFromCollection()

```ts
function removeFromCollection(itemKey, collectionKey): Promise<Change | null>;
```

Asks the user to take an item out of a collection (the item stays in the library).

**Permission**: `zotero:remove`, and the user's yes each time.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |
| `collectionKey` | `string` |

## Returns

`Promise`\<[`Change`](../Change.md) \| `null`\>

## Example

```ts
const removed = await zk.zotero.removeFromCollection(item.key, inbox.key); // Change | null
```
