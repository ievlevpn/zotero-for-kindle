# Feed Riffle (skeleton)

The data flow of [Feed Riffle](https://github.com/ievlevpn/zotero-feed-riffle) for the Kindle,
written to test the plugin API: if this needs a workaround, the API is wrong. **No screens yet**
(not in this version of the app): the deck screen is a placeholder, and the functions it will call —
`discard`, `skip`, `keep`, `keepRecent`, `replaceOld`, `undo` — are exported from `src/main.ts`.

## What it does

- **Fetches feeds itself.** Zotero's feed reader isn't on the Kindle, so the background task
  (after a sync, at most hourly) fetches each feed in `settings.json`, parses RSS 2.0, RSS 1.0 or
  Atom, and adds new entries to a deck kept in the plugin's storage. It parses with
  [txml](https://www.npmjs.com/package/txml) 6.0.3 (pinned; no dependencies, no Node or DOM APIs),
  bundled into `main.js`.
- **Marks what you already have.** A card whose DOI or arXiv id is in the library is marked with
  those items (`inLibrary`); the arXiv version suffix is ignored, so a `v2` announcement finds the
  `v1` you saved. A card already dealt or decided isn't dealt again.
- **discard** — the card is decided and gone. **skip** — to the back of the deck.
- **keep({ collection, tags, note })** — creates the item in Zotero, filed and tagged in one change,
  with the note as a child note. **keepRecent(n)** files into the n-th most recently used collection
  (keys 1–9 on the desktop).
- **replaceOld(old, new)** — copies the old item's notes onto the new one, then asks you to move the
  old item to Zotero's trash. Say no and the copies are withdrawn; both items stay as they were.
- **undo** — puts the last card back. Undoing a keep withdraws its changes if they haven't synced
  (the new item, its note, its filing, and the old item's trashing); once synced, a new item can
  only be trashed (you are asked) and a trashed old item is restored in Zotero.

## Permissions

| Permission | Why |
|---|---|
| `net:*` | feeds can be on any host (arXiv, journals, blogs) |
| `library:read` | find items by DOI or arXiv id; list collections |
| `notes:read` | read the old item's notes, to copy them when replacing it |
| `zotero:create-items` | create the kept item |
| `zotero:collections` | file it into a collection |
| `zotero:tags` | tag it |
| `zotero:notes` | add your note, and the copied notes |
| `zotero:remove` | ask to trash the old item when replacing it (and, undoing a synced keep, the new one) — your yes every time |
| `background` | fetch the feeds after a sync |

## Settings

`settings.json`, next to `plugin.json`:

```json
{
  "feeds": ["https://rss.arxiv.org/rss/math.PR", "https://www.example.org/atom.xml"],
  "tags": ["riffled"]
}
```

- `feeds` — RSS or Atom addresses.
- `tags` — added to every kept item, besides those chosen when keeping it.

## Build

`npm ci` here (for txml), then `node build.mjs`; `sdk/check.sh` does both.
