[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / close

# Function: close()

```ts
function close(result?): void;
```

Closes the screen whose `onEvent` is running, when that call ends. The screen below gets a
`result` event with `result` (copied as JSON). Throws an `Error` outside `onEvent`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `result?` | `unknown` |

## Returns

`void`

## Example

```ts
if (e.kind === "tap" && e.id === "file") { await fileIt(); zk.ui.close("Filed into Probability"); }
```
