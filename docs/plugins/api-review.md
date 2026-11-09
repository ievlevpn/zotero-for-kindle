# Plugin API v0 — review

For Pavel, before the runtime is built. Read with `sdk/types/zk.d.ts` (the API) and the three
plugins in `sdk/examples/`. All three type-check against `zk.d.ts`, their `plugin.json` validate,
and they build (`sh sdk/check.sh`). They aren't run: no runtime exists yet. (Highlights to
Markdown and the Feed Riffle flows were smoke-tested in Node against a stubbed `zk`.)

## The API in ten lines

```ts
zk.plugin, zk.settings                     // who am I; settings.json, typed by the plugin
console.log/warn/error                     // the plugin's log (zk.log is an alias)
zk.storage.get/set/delete/keys             // private JSON key-value store
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
  by origin: a Kindle highlight rewrites its book at once; a sync's highlights are noted and
  written on `syncFinished`; an hourly `background` run catches what was missed (and on its first
  run writes the whole library, since the first sync fires no highlight events). Item →
  attachments → annotations, `zk.files`, a `zk.storage` index of files written, and the
  five-line `serially` helper for state shared between handlers.
- **Feed Riffle skeleton** (`feed-riffle-skeleton/`). The hard case. `net:*` fetch with a
  per-feed timeout, a bundled npm XML parser (txml 6.0.3), `findByIdentifier` ignoring arXiv
  versions, a deck in storage, `keep` as one `createItem` (filed and tagged) plus `addNote`,
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
   read-modify-write of storage must be run one after another (the examples' `serially`).
9. **`zk.PermissionError` / `zk.LimitError` were constructible.** Now `private constructor()`:
   plugins test them with `instanceof`, only the app makes them.
10. **`console.*` vs `zk.log`.** Decided: `console.log/warn/error` is preferred (standard, has
    levels, what bundled libraries call); `zk.log` is documented as its alias. Every example in
    `zk.d.ts`, the template and the three plugins use `console`. (Removing `zk.log` — see below.)

Also changed outside `zk.d.ts`: `sdk/check.sh` runs `npm ci` in an example that has a
`package-lock.json` (Feed Riffle's txml), and `.gitignore` ignores every `sdk/**/node_modules/`.

**The spec's "The API (v0)" section still says `trash(itemKeys)` and `Promise<boolean>`**; it
should follow whatever you approve here.

## Kept as they are, on purpose

- **Annotations by attachment, not by item.** Exporting a book is a two-step loop (attachments,
  then each one's annotations). Three lines, and it matches Zotero's model; events already carry
  both keys.
- **Citation key from `extra`.** The plugin parses Better BibTeX's `Citation Key:` line itself.
  An `Item.citationKey` would be one less regex; left for you (see below).
- **`Change` for all writes, `cancel` by id.** No `undo()` stack in the API: plugins keep their
  own history (Feed Riffle's undo needs to restore its deck too, which the app can't know).

## Open questions for you

1. **Should the runtime serialise a plugin's calls**, so no plugin needs `serially`? Simpler
   to write, but a 30-second background run would hold up an event or an action. Both stateful
   examples needed the same helper, which is a smell. A middle way: a `zk.storage.update(key,
   fn)` that runs updates in order.
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
