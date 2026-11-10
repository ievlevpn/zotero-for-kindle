[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / deck

# Function: deck()

```ts
function deck(deck): DeckBlock;
```

Shows one card at a time; swiping it left or right sends a `swipe` event (the app turns the
card with the reader's page-turn feel when the next render shows another). With no card,
shows `empty`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `deck` | `Omit`\<[`DeckBlock`](DeckBlock.md), `"kind"`\> |

## Returns

[`DeckBlock`](DeckBlock.md)

## Example

```ts
zk.ui.deck({ id: "cards", card: top && zk.ui.card({ title: top.title }), empty: "Nothing left to riffle" });
```
