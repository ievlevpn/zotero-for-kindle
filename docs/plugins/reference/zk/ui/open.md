[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / open

# Function: open()

```ts
function open(screenId, params?): void;
```

Opens one of the plugin's screens over the current one, with `params` (copied as JSON) for
its `render` and `onEvent`. Takes effect when the current call ends. Only in answer to the
user: from `onEvent` or an action's `run`; elsewhere (the background task, an event
handler) it throws an `Error`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `screenId` | `string` |
| `params?` | `unknown` |

## Returns

`void`

## Example

```ts
onEvent: (e) => {
  if (e.kind === "swipe" && e.direction === "right") zk.ui.open("file", { cardId: top.id });
}
```
