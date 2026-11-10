# Plugin API changelog

A plugin says which API it was written for in `plugin.json` (`"api": 0`), and the app loads only
plugins whose API it runs; others are listed as *Incompatible*.

**v0 is unstable.** Names and shapes may still change until v1. Every change is listed here, newest
first, with what to do about it.

## v0, part 2 — screens, entry points, the result pop-up (unreleased)

Still API 0 (v0 is unstable); plugins written for 0.3.0 keep loading, except a `zk.screen` call
(below). Reference: [reference/](reference/README.md).

```ts
zk.screen(id, { title, main?, render(params), onEvent?(event, params) })  // a plugin screen
zk.ui.column/row/text/card/deck/list/search/field/chips/button/buttons/divider/space/notice
zk.ui.open(screenId, params?), zk.ui.close(result?), zk.ui.refresh()
zk.ui.Event: open, tap, select, swipe, change, submit, chips, result, back
run(target, again?) → string | { text, actions?: [{ id, title }] (≤ 2) } | void
```

- **`zk.screen` changed** (it was provisional): `zk.screen(id, render)` is now
  `zk.screen(id, { title, main?, render, onEvent? })`. `render(params)` returns building blocks
  (`zk.ui.Block`), may be async, and has 1 s; `onEvent(event, params)` has an action's time (at
  least 30 s) and may change Zotero and ask for removals. `main: true` lists the screen in the
  Plugins launcher on Home. `ScreenParams` and `ScreenContent` are gone (`ScreenSpec<P>` instead;
  `P` is what `zk.ui.open` passed). Caps: 2,000 blocks and 256 KB of text a tree.
- **`zk.ui`**: one helper per block, each returning a plain object (`{ kind: "text", … }`);
  children skip falsy values, so `cond && block` works. `open`/`close` push and pop the plugin's
  screens (the screen below gets a `result` event); `refresh` re-renders after work outside
  `onEvent`.
- **Entry points are shown**: selection actions under *More…* in the selection bar, item actions
  in the item details page's *Plugins* section. Their notes "not shown in this version yet" are
  gone.
- **Action results**: `run` may return `{ text, actions }`; a button re-runs the action with
  `again = { action, text }` (the button and the text shown). `ActionResult` gained
  `ActionReply`; `run` gained its second argument.
- Budgets: a screen's `render` 1 s; `onEvent` and actions at least 30 s.

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
zk.selectionAction/itemAction/screen       // recorded here; v0 part 2 shows them (see above)
zk.PermissionError, zk.LimitError          // what the app rejects with
```

Standard globals besides ES2023: `setTimeout`/`clearTimeout`, `URL`, `URLSearchParams`,
`TextEncoder`, `TextDecoder` (UTF-8 only).

Permissions: `library:read`, `annotations:read`, `fulltext:read`, `notes:read`, `net:<host>`,
`net:*.<domain>`, `net:*`, `zotero:create-items`, `zotero:collections`, `zotero:tags`,
`zotero:notes`, `zotero:remove`, `background`.

Limits: event handlers 5 s, the background task 30 s, actions at least 30 s; 32 MB of memory a
plugin, 96 MB for all together; `zk.fetch` 10 MB a response, 30 s a request, 8 under way;
`zk.storage` 1 MB a value, 16 MB in all; `zk.files` 16 MB a file, 64 MB in all; no writes
while the Kindle has less than 50 MB free; 2 s of quiet run time a minute. See [Concepts](concepts.md).

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
