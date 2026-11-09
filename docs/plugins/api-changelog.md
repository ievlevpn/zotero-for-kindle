# Plugin API changelog

A plugin says which API it was written for in `plugin.json` (`"api": 0`), and the app loads only
plugins whose API it runs; others are listed as *Incompatible*.

**v0 is unstable.** Names and shapes may still change until v1. Every change is listed here, newest
first, with what to do about it.

## v0 — the first plugin API (app 0.3.0)

### What it has

The reference, generated from `sdk/types/zk.d.ts`, is in [reference/](reference/README.md).

```ts
zk.plugin, zk.settings                     // who am I; settings.json, typed by the plugin
console.log/warn/error                     // the plugin's log
zk.storage.get/set/update/delete/keys      // private JSON store; update = ordered read-modify-write
zk.files.read/write/list/remove            // its own data/ folder, for exports
zk.fetch(url, { method, headers, body, timeout })   // web fetch: HTTPS, declared hosts, capped
zk.library.items/item/search/findByIdentifier/collections/tags/attachments/annotations/
           recentAnnotations/notes/fullText          // plain objects, lazy iterables where long
zk.zotero.createItem/addToCollection/addTag/addNote  // → Change { key, changeId }, at once
zk.zotero.trash/removeFromCollection/removeTag/deleteNote  // → Change | null, after the user's yes
zk.zotero.cancel(changeId), pending()      // withdraw / list its unsent changes
zk.on(event, handler)                      // highlightCreated/Changed/Deleted, bookOpened,
                                           // bookFinished, itemMarkedRead/Unread, syncFinished
zk.background(task), zk.setBadge(text)     // after a sync, at most hourly; a short badge
zk.selectionAction/itemAction/screen       // recorded, not shown yet (plugin screens come later)
zk.PermissionError, zk.LimitError          // what the app rejects with
```

Standard globals besides ES2023: `setTimeout`/`clearTimeout`, `URL`, `URLSearchParams`,
`TextEncoder`, `TextDecoder` (UTF-8 only).

Permissions: `library:read`, `annotations:read`, `fulltext:read`, `notes:read`, `net:<host>`,
`net:*.<domain>`, `net:*`, `zotero:create-items`, `zotero:collections`, `zotero:tags`,
`zotero:notes`, `zotero:remove`, `background`.

Limits: event handlers 5 s, the background task 30 s, actions at least 30 s; 32 MB of memory a
plugin, 96 MB for all together; `zk.fetch` 10 MB a response, 30 s a request, 8 under way;
`zk.storage` 1 MB a value, 16 MB in all; `zk.files` 16 MB a file. See [Concepts](concepts.md).

### Changed before release

The API was reviewed against three plugins written for it (*Highlights to Markdown*, a Feed Riffle
skeleton, *Explain with AI*, in `sdk/examples/`) before the app ran any of it. What changed then,
for anyone who read an early draft:

- **Removals resolve `Change | null`** (they resolved `boolean`), so an agreed removal has a
  `changeId` and can be withdrawn while unsent. `if (await zk.zotero.trash(key))` reads as before.
- **`zk.zotero.trash(itemKey)`** takes one item (it took a list), like the other removals. Removals
  asked together are confirmed together, in one list.
- **`zk.zotero.cancel`** withdraws any unsent change of the plugin's, removals included (the item
  comes back out of the trash).
- **`createItem`** needs `zotero:collections` or `zotero:tags` only when those lists aren't empty.
  Its `PendingChange` lists the `collections` and `tags` it was given.
- **`zk.storage.update(key, fn)`** added: the way to do read-modify-write. All of a plugin's
  storage writes (`set`, `delete`, `update`) apply in the order they were asked; a write inside an
  update's function rejects at once. `get` and `update` hand back fresh copies.
- **`zk.log` removed.** `console.log/warn/error` is the one way to log.
- **`Item.citationKey`** added: Zotero's own field, or Better BibTeX's `Citation Key:` line in
  *Extra*.
- **`Response.text()`** is always UTF-8, as on the web; **`Response.bytes()`** added for other
  encodings. **`Response.headers`** is a read-only web `Headers` (`get`, `has`, `entries`).
- **`RequestInit.timeout`** added (milliseconds, at most 30 000), so one slow host can't use up
  a background run.
- **`zk.library.items()`** has no `text` filter: `zk.library.search(text)` is the one way to
  search.
- **`zk.PermissionError` and `zk.LimitError`** can't be constructed by plugins; test with
  `instanceof`.
- **Time budgets** count from when a call starts running: not time queued behind other plugins,
  not time waiting for the user's answer. Actions get at least 30 s.
- **A call stopped by a limit** has the work it started cancelled, and every pending promise of
  that work rejects with `zk.LimitError`, so nothing hangs. If the stop cut into running code,
  the plugin restarts (module variables start over; storage stays).
- **Timers**: a timer that fires after the call that set it has ended runs as a call of its own,
  with 5 seconds.
- **Memory** is 32 MB a plugin and 96 MB for all plugins together, counted on what they use.
- **`zk.fetch` with `net:*` or `net:*.<domain>`** doesn't reach hosts on the local network; declare them by their exact name. Addresses (`net:127.0.0.1`, `net:0x7f.1`) and `net:zotero.org` can't be declared.
