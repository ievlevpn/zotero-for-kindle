[Plugin API v0](../README.md) / [zk](../zk.md) / LimitError

# Class: LimitError

Thrown (or rejected with) when the plugin goes over one of the app's limits; the message names
it. `time`, `memory` and `stack` stop the running handler or task; the fetch limits
(`responseSize`, `responseTime`, `fetches`) only fail that fetch.

`time` is the call's budget: event handlers 5 s, the background task 30 s, actions and a
screen's `onEvent` at least 30 s, a screen's `render` 1 s. It counts from when the call starts running; time queued behind other plugins' calls
and time waiting for the user to answer a confirmation never count. `memory` is 32 MB for a
plugin, and 96 MB for all plugins together: a call whose allocation would pass either is
stopped.

When a call is stopped, the work it started in the app (fetches, library reads, storage and
file operations, timers) is cancelled, and every promise of that work still pending rejects
with a `LimitError` — none is left pending forever. So [Storage.update](Storage.md#update)'s queue, or any
queue a plugin keeps itself, moves on to the next job instead of hanging. Changes to Zotero
already asked for stay asked for (each resolved at once); a removal still waiting for the
user's answer is withdrawn. Code that catches the `LimitError` can't start new work for the
stopped call: such calls reject at once with the same `LimitError`.

After a limit stop that cut into running code (not just a call left waiting too long), the
plugin is restarted: `main.js` runs afresh in a new runtime, so module-level variables start
over. Keep anything that must survive in [zk.storage](storage-1.md). Every stop counts as a failure;
three failures in a row turn the plugin off.

## Example

```ts
try {
  const res = await zk.fetch(bigFeedUrl);
} catch (e) {
  if (e instanceof zk.LimitError && e.limit === "responseSize") console.log("feed too large");
  else throw e;
}
```

## Extends

- `Error`

## Properties

| Property | Modifier | Type | Description | Overrides | Inherited from |
| ------ | ------ | ------ | ------ | ------ | ------ |
| <a id="cause"></a> `cause?` | `public` | `unknown` | - | - | `Error.cause` |
| <a id="limit"></a> `limit` | `readonly` | \| `"time"` \| `"memory"` \| `"stack"` \| `"responseSize"` \| `"responseTime"` \| `"fetches"` \| `"timers"` \| `"handlers"` | Which limit: run time, memory, stack depth, a fetch's response size or duration, too many fetches under way at once, too many timers pending ([setTimeout](../setTimeout.md)), or too many handlers for one event ([on](on.md)). | - | - |
| <a id="message"></a> `message` | `public` | `string` | - | - | `Error.message` |
| <a id="name"></a> `name` | `readonly` | `"LimitError"` | - | `Error.name` | - |
| <a id="stack"></a> `stack?` | `public` | `string` | - | - | `Error.stack` |
