[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / row

# Function: row()

```ts
function row(children, opts?): RowBlock;
```

Places blocks side by side, sharing the width (e.g. a label and a button). Wraps nothing:
keep it to two or three short blocks.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `children` | [`Child`](Child.md)[] |
| `opts?` | \{ `gap?`: [`Gap`](Gap.md); \} |
| `opts.gap?` | [`Gap`](Gap.md) |

## Returns

[`RowBlock`](RowBlock.md)

## Example

```ts
zk.ui.row([zk.ui.text("math.PR · 12 left", { style: "label" }), zk.ui.button({ id: "feeds", title: "Feeds" })]);
```
