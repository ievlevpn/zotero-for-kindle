[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / button

# Function: button()

```ts
function button(button): ButtonBlock;
```

A button, full width; tapping sends a `tap` event. `primary` for the screen's main action.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `button` | `Omit`\<[`ButtonBlock`](ButtonBlock.md), `"kind"`\> |

## Returns

[`ButtonBlock`](ButtonBlock.md)

## Example

```ts
zk.ui.button({ id: "file", title: "File", primary: true });
```
