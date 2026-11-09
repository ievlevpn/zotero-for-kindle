[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / item

# Function: item()

```ts
function item(key): Promise<Item | null>;
```

One item by its key, or `null` if there is none (or it is in the trash).

**Permission**: `library:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `key` | `string` |

## Returns

`Promise`\<[`Item`](../Item.md) \| `null`\>

## Example

```ts
const item = await zk.library.item(annotation.itemKey);
```
