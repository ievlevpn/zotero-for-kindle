[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / annotations

# Function: annotations()

```ts
function annotations(attachmentKey): Promise<Annotation[]>;
```

An attachment's annotations (highlights, notes, …) in reading order.

**Permission**: `annotations:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `attachmentKey` | `string` |

## Returns

`Promise`\<[`Annotation`](../Annotation.md)[]\>

## Example

```ts
const highlights = (await zk.library.annotations(pdf.key)).filter((a) => a.type === "highlight");
```
