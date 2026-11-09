# Plugin API v0 — review

For Pavel, before the runtime is built. **Approved 2026-11-09 with the decisions recorded at the
end**; the text above that section describes the review as it was put to him, updated where a
decision changed the API. Read with `sdk/types/zk.d.ts` (the API) and the three
plugins in `sdk/examples/`. All three type-check against `zk.d.ts`, their `plugin.json` validate,
and they build (`sh sdk/check.sh`). They aren't run: no runtime exists yet. (Highlights to
Markdown and the Feed Riffle flows were smoke-tested in Node against a stubbed `zk`.)

## The API in ten lines

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
zk.on(event, handler), zk.background(task), zk.setBadge(text), zk.selectionAction/itemAction/screen
```

## What each example shows

- **Highlights to Markdown** (`highlights-to-markdown/`, the shipped teaching example). Events
  by origin: every change is noted in a "to do" list first; a Kindle highlight's book is then
  written at once; `syncFinished` writes a few noted books; an hourly `background` run drains the
  list and catches what was missed (its first run notes the whole library, since the first sync
  fires no highlight events; a cut-short run resumes next time). Item → attachments →
  annotations, `item.citationKey`, `zk.files`, and a `zk.storage` index of files written, changed
  only through `zk.storage.update`, one book per update.
- **Feed Riffle skeleton** (`feed-riffle-skeleton/`). The hard case. `net:*` fetch with a
  per-feed timeout, a bundled npm XML parser (txml 6.0.3), `findByIdentifier` ignoring arXiv
  versions, a deck in storage changed through `zk.storage.update`, `keep` as one `createItem` (filed and tagged) plus `addNote`,
  `replaceOld` (copy notes, ask to trash, withdraw the copies if the user says no), and `undo`
  that cancels unsent changes — including the old item's trashing — or asks to trash if synced.
- **Explain with AI** (`explain-with-ai/`). A selection action, typed settings with defaults,
  `net:api.anthropic.com` only, a JSON POST with error handling, a string returned for the
  pop-up. It needs nothing else: the API is small for the small case.

## Where the API was clunky, and what changed (all in `zk.d.ts`)

1. **Undo of a replace was impossible.** Removals resolved `boolean`, so a trash had no
   `changeId` and Feed Riffle couldn't undo "keep + trash the old one" — yet the desktop plugin's
   `u` puts the old item straight back. **Now every `zk.zotero` call resolves a `Change`;
   removals resolve `Change | null`** (`null`: declined or not allowed). `if (await
   zk.zotero.trash(key))` still reads as before.
2. **`cancel` was silent on removals.** Now stated: `cancel` withdraws any unsent change of the
   plugin's — an add is taken back, an agreed removal is put back (the item comes out of the
   trash). No confirmation: nothing left the Kindle. Undoing a *sent* removal stays impossible
   from a plugin (restoring is an edit); the doc says the user restores it in Zotero.
3. **`trash(itemKeys[])` was the odd one out.** Now `trash(itemKey)`, like the other three
   removals. Batching was already there: removals asked together are confirmed in one list —
   `Promise.all(keys.map(zk.zotero.trash))` — now said to be one answer for all. Consistent, and
   one way to do it.
4. **`PendingChange` for `createItem` lost its collections and tags.** Added `collections` and
   `tags` (`[]` if none), so `pending()` shows what cancelling would withdraw.
5. **`createItem`'s permissions with empty options** were ambiguous (`tags: []` needs
   `zotero:tags`?). Now: only when non-empty — so `keep` can pass its lists unconditionally.
6. **`Response.text()` promised other charsets**, while the only decoder is UTF-8. Now `text()`
   is always UTF-8, as on the web, and **`bytes()`** was added (also on the web) for anything
   else; the doc shows decoding a Latin-1 feed.
7. **One slow feed could eat the 30-second background run** (a fetch could take 30 s). Added
   **`RequestInit.timeout`** (ms, at most the 30 s cap; rejects with `LimitError
   "responseTime"`). The web's way, `AbortSignal.timeout()`, would mean providing AbortSignal;
   one number is simpler.
8. **State shared between calls had no stated rules.** Both stateful examples need to know
   whether module variables survive between calls and whether calls interleave. The header now
   says: the module stays loaded while the plugin is on; calls interleave at `await`, so
   read-modify-write of storage must be run one after another — after review, with
   `zk.storage.update` (see Decisions).
9. **What a stopped call leaves behind was unstated.** Now: its pending work is cancelled and
   rejects with `zk.LimitError`, so nothing hangs (see open question 1).
10. **`zk.PermissionError` / `zk.LimitError` were constructible.** Now `private constructor()`:
   plugins test them with `instanceof`, only the app makes them.
11. **`console.*` vs `zk.log`.** `console.log/warn/error` is the way to log (standard, has
    levels, what bundled libraries call). After review `zk.log` is removed (see Decisions).

Also changed outside `zk.d.ts`: `sdk/check.sh` runs `npm ci` in an example that has a
`package-lock.json` (Feed Riffle's txml), and `.gitignore` ignores every `sdk/**/node_modules/`.

The spec's "The API (v0)" section was updated to match after the review.

## Kept as they are, on purpose

- **Annotations by attachment, not by item.** Exporting a book is a two-step loop (attachments,
  then each one's annotations). Three lines, and it matches Zotero's model; events already carry
  both keys.
- **`Change` for all writes, `cancel` by id.** No `undo()` stack in the API: plugins keep their
  own history (Feed Riffle's undo needs to restore its deck too, which the app can't know).

## Open questions for you (all resolved: see Decisions)

1. **Should the runtime serialise a plugin's calls**, so no plugin needs `serially`? Simpler
   to write, but a 30-second background run would hold up an event or an action. Both stateful
   examples needed the same helper, which is a smell. A middle way: a `zk.storage.update(key,
   fn)` that runs updates in order. Either way, a plugin-side queue is only safe if a stopped
   call can't leave it waiting forever. **Ruled (controller, review round 1):** when a call is
   stopped by a limit, the app cancels the work it started and every pending promise of that work
   rejects with `zk.LimitError`; now stated in `zk.d.ts` (`LimitError`, `zk.on`,
   `zk.background`). The examples also keep each queued job short (one book), so an event
   handler never waits behind a whole background run.
2. **Is a handler's time budget wall-clock or JavaScript time?** A Kindle highlight handler
   waiting behind the background run (5 s budget, 30 s run) fails if waiting counts. Proposal:
   count time running JavaScript, plus a generous wall-clock cap.
3. **Remove `zk.log`?** One obvious way would say yes; the spec lists both. Kept for now.
4. **Add `Item.citationKey`** (from Zotero's own field or the `Extra` line)?
5. **Action budgets (part 2).** Explain with AI waits on a remote model for several seconds; the
   action budget should allow at least the 30-second fetch cap.
6. **Removals from a plugin's own screen.** Feed Riffle's replace and synced-undo ask for a
   trash from functions a screen will call; that counts as "in answer to something the user just
   did" only once part 2 routes screen taps to the plugin. Fine for part 1, where they can't run.

## Decisions (Pavel, 2026-11-09)

Pavel approved the API with these answers. All are applied in `zk.d.ts`, the examples, the
test fixture and `sdk/check.sh` (the spec's API section already points to them).

1. **Serialising calls (open question 1): no.** Calls keep interleaving at `await`. Instead,
   **`zk.storage.update<T>(key, fn)`**: `fn` gets the stored value (or `undefined`) and returns
   the new one; returning `undefined` deletes the key; it resolves with what was stored. A
   plugin's updates run strictly in order (a per-plugin queue in the app), and `fn` may be async.
   It is documented as the way to do read-modify-write. Rulings that followed (controller):
   `set` and `delete` share the same per-plugin queue, so **all storage writes apply in the order
   they were asked**; a storage write (`update`, `set` or `delete`, any key) called inside an
   `update`'s `fn` **rejects at once** with an Error saying it can't be called there, instead of
   waiting on itself until the budget runs out; `get` and `update` hand back fresh copies. Both stateful examples now use it, and
   `serially` is gone from both: nothing in them needed to sequence more than storage. The
   controller's ruling from review round 1 stands: a call stopped by a limit has its pending work
   cancelled and rejected with `zk.LimitError`, so the queue never hangs.
2. **Handler budgets (open question 2): ruled by the controller.** A call's budget counts from
   when it starts running; time queued behind other plugins' calls doesn't count, and neither does
   time waiting for the user's confirmation. Stated in the header of `zk.d.ts`, in `zk.on`,
   `zk.background`, the actions and `LimitError`.
3. **`zk.log` (open question 3): removed.** `console.*` is the one way to log. The test fixture
   and `check.sh`'s stub now use `console` too.
4. **`Item.citationKey` (open question 4): added**, `string | null`: Zotero's own `citationKey`
   field when present, else Better BibTeX's `Citation Key:` line in *Extra*. Highlights to
   Markdown uses it instead of parsing *Extra*.
5. **Action budgets (open question 5):** `selectionAction` and `itemAction` document at least
   30 s for `run` (the fetch cap); time waiting for the user's confirmation never counts.
6. **Removals from a plugin's own screen (open question 6):** no change; it is part 2's to settle
   when screens route taps to the plugin.
