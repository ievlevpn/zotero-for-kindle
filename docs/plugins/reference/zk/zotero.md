[Plugin API v0](../README.md) / [zk](../zk.md) / zotero

# zotero

Asks the app to change the Zotero library. Plugins never write to Zotero themselves: the app
records each change, shows it in the library at once, and the sync sends it to Zotero later
(without ever overwriting an edit made elsewhere). Settings → Plugins → *What it changed*
lists every change a plugin asked for and what became of it.

Every call changes one thing and resolves with a [Change](Change.md): the key of what changed and a
`changeId` to [cancel](zotero/cancel.md) it while it is still unsent.

- **Adding** (items, collections, tags, notes) is allowed by its permission and resolves at once.
- **Removing** (trash, remove from a collection, remove a tag, delete a note) needs the user's
  yes every time: the app shows its own confirmation and the call resolves with the change
  (done) or `null` (declined, or not allowed right now) — so `if (await zk.zotero.trash(key))`
  reads as "if it was trashed". Removals are only asked in answer to something the user just
  did (an action, a screen, an event they caused on the Kindle); from a background task or an
  event that came in with a sync they resolve `null` without asking. Removals asked together
  (before awaiting any of them, e.g. with `Promise.all`) are confirmed together, in one list,
  with one answer for all. One call asks at most 3 questions; further removals in that call
  resolve `null` without asking (the log says so). Nothing is ever deleted for good except a note this plugin created:
  items go to Zotero's trash.
- **Editing** existing data (a title, a note, an annotation) is not possible in API v0.
- **Group libraries**: a change goes to the library of what it changes (a new item to its
  collections' library; none means My Library). It is refused, with a message saying why, when
  that group is read-only for the user, switched off on the Kindle or no longer available, and
  when it mixes libraries (collections of two libraries, or an item filed into another
  library's collection). See [zk.library.libraries](library/libraries.md) (`editable`).

## Functions

- [addNote](zotero/addNote.md)
- [addTag](zotero/addTag.md)
- [addToCollection](zotero/addToCollection.md)
- [cancel](zotero/cancel.md)
- [createItem](zotero/createItem.md)
- [deleteNote](zotero/deleteNote.md)
- [pending](zotero/pending.md)
- [removeFromCollection](zotero/removeFromCollection.md)
- [removeTag](zotero/removeTag.md)
- [trash](zotero/trash.md)
