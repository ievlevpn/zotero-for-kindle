[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / text

# Function: text()

```ts
function text(text, opts?): TextBlock;
```

Text in one of four styles; `"\n"` breaks a line. `spans` mark bold and italic stretches.
`lines` caps it, ending with "…".

## Parameters

| Parameter | Type |
| ------ | ------ |
| `text` | `string` \| [`Span`](Span.md)[] |
| `opts?` | \{ `lines?`: `number`; `style?`: [`TextStyle`](TextStyle.md); \} |
| `opts.lines?` | `number` |
| `opts.style?` | [`TextStyle`](TextStyle.md) |

## Returns

[`TextBlock`](TextBlock.md)

## Example

```ts
zk.ui.text("Gaussian random fields", { style: "title", lines: 2 });
zk.ui.text(["Filed into ", { text: "Probability / SPDEs", bold: true }], { style: "meta" });
```
