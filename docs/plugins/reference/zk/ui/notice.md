[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / notice

# Function: notice()

```ts
function notice(text): NoticeBlock;
```

A quiet one-line message: the outcome of the last step, a hint.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `text` | `string` |

## Returns

[`NoticeBlock`](NoticeBlock.md)

## Example

```ts
zk.ui.notice("Discarded. 3 cards skipped for later.");
```
