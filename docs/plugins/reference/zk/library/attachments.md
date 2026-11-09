[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / attachments

# Function: attachments()

```ts
function attachments(itemKey): Promise<Attachment[]>;
```

An item's attachments (PDFs, EPUBs, …), whether downloaded to the Kindle or not.

**Permission**: `library:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |

## Returns

`Promise`\<[`Attachment`](../Attachment.md)[]\>

## Example

```ts
const pdf = (await zk.library.attachments(item.key)).find((a) => a.downloaded);
```
