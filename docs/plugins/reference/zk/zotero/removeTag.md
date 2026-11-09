[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / removeTag

# Function: removeTag()

```ts
function removeTag(itemKey, tag): Promise<Change | null>;
```

Asks the user to remove a tag from an item.

**Permission**: `zotero:remove`, and the user's yes each time.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |
| `tag` | `string` |

## Returns

`Promise`\<[`Change`](../Change.md) \| `null`\>

## Example

```ts
await zk.zotero.removeTag(item.key, "to-read");
```
