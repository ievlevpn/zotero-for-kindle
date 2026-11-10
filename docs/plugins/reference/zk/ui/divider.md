[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / divider

# Function: divider()

```ts
function divider(): DividerBlock;
```

A thin line between sections.

## Returns

[`DividerBlock`](DividerBlock.md)

## Example

```ts
zk.ui.column([summary, zk.ui.divider(), feedList]);
```
