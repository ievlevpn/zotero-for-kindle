[Plugin API v0](../README.md) / [zk](../zk.md) / on

# Function: on()

```ts
function on<K>(event, handler): void;
```

Runs `handler` whenever `event` happens. Register handlers when the plugin loads (at the top
level of `main.js`). A handler has 5 seconds from when it starts running (not counting time
queued behind other plugins or waiting for the user's confirmation); going over stops it
(see [LimitError](LimitError.md): what it started is cancelled). An error it throws is logged with
its stack.

A plugin may register at most 100 handlers for each event; one more throws a
[LimitError](LimitError.md) `"handlers"` (it isn't registered).

Every event says where the change was made: `origin: "kindle"` (the user did it on this
Kindle, one event per change, as it happens) or `origin: "zotero"` (it came in with a sync:
one event per kind per sync, listing everything). So events that can arrive with a sync carry
a list — `annotations` or `items` — with one entry when the change was made on the Kindle.
The first full sync of a library fires only `syncFinished` (with `initial: true`). That holds for
each group library too: the sync in which a group is first downloaded (newly joined, or switched
on again after its copy was removed) ends with `initial: true`, even when My Library had synced
before, so a plugin that keeps an index rebuilds it then.

**Permission**: what the event carries — `annotations:read` for `highlight…` events,
`library:read` for `book…` and `itemMarked…` events; `syncFinished` needs none.

## Type Parameters

| Type Parameter |
| ------ |
| <a id="onk"></a> `K` *extends* keyof [`Events`](Events.md) |

## Parameters

| Parameter | Type |
| ------ | ------ |
| `event` | `K` |
| `handler` | (`e`) => `void` \| `Promise`\<`void`\> |

## Returns

`void`

## Examples

```ts
zk.on("highlightCreated", async ({ origin, annotations }) => {
  for (const a of annotations) console.log(origin, a.text);
});
```

```ts
zk.on("syncFinished", ({ initial }) => {
  if (!initial) console.log("synced");
});
```
