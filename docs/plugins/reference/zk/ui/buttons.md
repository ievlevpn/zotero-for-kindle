[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / buttons

# Function: buttons()

```ts
function buttons(buttons): ButtonsBlock;
```

Up to 3 buttons in a row, sharing the width.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `buttons` | ([`Falsy`](Falsy.md) \| `Omit`\<[`ButtonBlock`](ButtonBlock.md), `"kind"`\>)[] |

## Returns

[`ButtonsBlock`](ButtonsBlock.md)

## Example

```ts
zk.ui.buttons([{ id: "discard", title: "Discard" }, { id: "skip", title: "Skip" }, { id: "keep", title: "Keep", primary: true }]);
```
