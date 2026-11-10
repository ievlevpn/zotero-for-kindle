[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / field

# Function: field()

```ts
function field(field): FieldBlock;
```

A text input: one line (Enter sends `submit`), or several (`lines` > 1; Enter makes a new
line). `change` events as for [search](search.md).

## Parameters

| Parameter | Type |
| ------ | ------ |
| `field` | `Omit`\<[`FieldBlock`](FieldBlock.md), `"kind"`\> |

## Returns

[`FieldBlock`](FieldBlock.md)

## Example

```ts
zk.ui.field({ id: "note", label: "Note", lines: 3 });
```
