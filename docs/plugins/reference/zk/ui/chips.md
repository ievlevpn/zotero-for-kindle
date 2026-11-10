[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / chips

# Function: chips()

```ts
function chips(chips): ChipsBlock;
```

Small chips in a wrapping row: tags, recent collections. With `select: "many"` (the
default) each tap toggles one; `"one"` keeps at most one selected; `"none"` makes them act
like small buttons. Every tap sends a `chips` event. A chip given as a string is its own id
and title.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `chips` | `Omit`\<[`ChipsBlock`](ChipsBlock.md), `"kind"`\> |

## Returns

[`ChipsBlock`](ChipsBlock.md)

## Example

```ts
zk.ui.chips({ id: "tags", chips: ["to-read", "spde"], selected: chosen });
zk.ui.chips({ id: "recent", select: "none", chips: recent.map((c) => ({ id: c.key, title: c.name })) });
```
