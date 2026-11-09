[Plugin API v0](README.md) / setTimeout

# Function: setTimeout()

```ts
function setTimeout(callback, ms?): number;
```

Runs `callback` after `ms` milliseconds, on the plugin thread. While the call that set it (an
event handler, the background task, an action) is still running, the callback counts against
that call's time budget; a timer firing after that call has finished runs as a call of its own,
with 5 seconds. Pending timers are dropped when the call that set them is stopped by a limit,
and when the plugin is turned off. Returns an id for [clearTimeout](clearTimeout.md). No permission.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `callback` | () => `void` |
| `ms?` | `number` |

## Returns

`number`

## Example

```ts
const pause = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));
for (const url of feeds) { await fetchFeed(url); await pause(500); } // be polite to the host
```
