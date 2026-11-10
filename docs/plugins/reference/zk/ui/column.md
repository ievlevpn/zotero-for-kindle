[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / column

# Function: column()

```ts
function column(children, opts?): ColumnBlock;
```

Stacks blocks top to bottom. The usual root of a screen.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `children` | [`Child`](Child.md)[] |
| `opts?` | \{ `gap?`: [`Gap`](Gap.md); \} |
| `opts.gap?` | [`Gap`](Gap.md) |

## Returns

[`ColumnBlock`](ColumnBlock.md)

## Example

```ts
zk.ui.column([zk.ui.text("Feeds", { style: "title" }), feedList], { gap: "medium" });
```
