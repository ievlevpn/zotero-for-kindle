[Plugin API v0](../README.md) / [zk](../zk.md) / background

# Function: background()

```ts
function background(task): void;
```

Registers the plugin's background task (one per plugin; registering again replaces it). The
app runs it after a successful sync, at most once an hour, while the Kindle is awake and
online; it never wakes a sleeping Kindle. A run has 30 seconds from when it starts running;
going over stops it (see [LimitError](LimitError.md): what it started is cancelled). Three failed runs
in a row
turn the plugin off. A background task can't show anything or ask the user: removals resolve
`null`; use [setBadge](setBadge.md) to say there is something to see.

**Permission**: `background`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `task` | () => `void` \| `Promise`\<`void`\> |

## Returns

`void`

## Example

```ts
zk.background(async () => {
  const fresh = await refreshFeeds();
  zk.setBadge(fresh ? `${fresh} new` : null);
});
```
