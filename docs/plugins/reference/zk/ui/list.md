[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / list

# Function: list()

```ts
function list(list): ListBlock;
```

Rows the user picks from, paged like the app's lists; tapping a row sends a `select`
event. `selected` marks a row as chosen.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `list` | `Omit`\<[`ListBlock`](ListBlock.md), `"kind"`\> |

## Returns

[`ListBlock`](ListBlock.md)

## Example

```ts
zk.ui.list({
  id: "collections",
  rows: matches.map((c) => ({ id: c.key, title: c.path.join(" / "), selected: c.key === chosen })),
  empty: "No collection matches",
});
```
