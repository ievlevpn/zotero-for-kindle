[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / card

# Function: card()

```ts
function card(card): CardBlock;
```

A card: title, a meta line, a body (paged with up/down swipes when long), and a badge
(e.g. "In library"). Shown alone, or one at a time in a [deck](deck.md).

## Parameters

| Parameter | Type |
| ------ | ------ |
| `card` | `Omit`\<[`CardBlock`](CardBlock.md), `"kind"`\> |

## Returns

[`CardBlock`](CardBlock.md)

## Example

```ts
zk.ui.card({ title: card.title, meta: "Adler, Taylor · 2024-01-03", body: card.summary, badge: "In library" });
```
