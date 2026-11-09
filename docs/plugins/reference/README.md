# Plugin API v0

Zotero for Kindle — plugin API v0.

**v0 is unstable**: names and shapes may still change; every change is listed in the API
changelog. A plugin declares the version it was written for in `plugin.json` (`"api": 0`).

A plugin is one ES module (`main.js`) run by the app on QuickJS-ng (ES2023). It is neither Node
nor a browser: there is no `require`, no `process`, no `window` or DOM. Rely on standard
ES2023 and on what this file declares: the one global `zk`, plus `console`, `setTimeout` /
`clearTimeout`, `URL`, `URLSearchParams`, `TextEncoder` and `TextDecoder` (only the members
declared below). Libraries bundled into `main.js` are fine if they need nothing else.

Everything is plain data: items, notes and annotations are JSON-like objects, the same shape
wherever they come from (a query, an event, an action). Everything that touches the library,
the network or files is `async`, because plugins run on their own thread and the screen never
waits for them.

The module is run once and stays loaded while the plugin is on, so module-level variables
last from one call to the next (but not across restarts: keep what must last in
[zk.storage](zk/storage-1.md)). Calls into the plugin (event handlers, the background task, actions) may
interleave at each `await`, as in any async JavaScript: a read-modify-write of storage that
awaits in between can lose another call's write, so do it with [zk.Storage.update](zk/Storage.md#update),
which runs a plugin's updates strictly one after another.

Each call has a time budget (event handlers 5 s, the background task 30 s, actions at least
30 s). It counts from when the call starts running: time queued behind other plugins' calls
doesn't count, and neither does time waiting for the user to answer a confirmation. A call
that goes over is stopped (see [zk.LimitError](zk/LimitError.md)).

What a plugin may do is declared in `plugin.json` under `"permissions"`. Calling something not
declared rejects (or throws) with a [zk.PermissionError](zk/PermissionError.md) that names what to add. Each
member below says which permission it needs.

## Example

```ts
// main.ts — log every book finished on the Kindle (needs "library:read")
zk.on("bookFinished", ({ item }) => {
  console.log(`Finished ${item.title} (${item.year ?? "n.d."})`);
});
```

## Namespaces

- [zk](zk.md)

## Classes

- [TextDecoder](TextDecoder.md)
- [TextEncoder](TextEncoder.md)
- [URL](URL.md)
- [URLSearchParams](URLSearchParams.md)

## Variables

- [console](console.md)

## Functions

- [clearTimeout](clearTimeout.md)
- [setTimeout](setTimeout.md)
