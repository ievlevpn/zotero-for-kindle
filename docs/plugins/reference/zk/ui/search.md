[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / search

# Function: search()

```ts
function search(search): SearchBlock;
```

A search box. Typing shows at once; `change` events are sent 700 ms after the user stops
typing (like the app's own searches), `submit` on Enter. See [FieldBlock.value](FieldBlock.md#value) for
replacing the text.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `search` | `Omit`\<[`SearchBlock`](SearchBlock.md), `"kind"`\> |

## Returns

[`SearchBlock`](SearchBlock.md)

## Example

```ts
zk.ui.search({ id: "find", placeholder: "Collection", focus: true });
```
