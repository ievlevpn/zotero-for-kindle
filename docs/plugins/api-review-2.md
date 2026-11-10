# Plugin API v0, part 2 — screens: review

For Pavel, before any C++. Read with `sdk/types/zk.d.ts` (section *Entry points* and *Screens*)
and `sdk/examples/feed-riffle/src/main.ts`. All three examples type-check, validate and build
(`sh sdk/check.sh`); Feed Riffle's whole flow was smoke-run in Node against a stubbed `zk`.

## The screen API in ten lines

```ts
zk.screen<P>(id, { title, main?, render(params: P), onEvent?(event, params: P) })
render → zk.ui.Block (may be async; 1 s)      onEvent → void (≥ 30 s, user-caused: may write, ask)
zk.ui.column/row(children, { gap })  text(text | spans, { style, lines })  notice  divider  space
zk.ui.card({ title, meta, body, badge })  deck({ id, card, empty })        // swipe left/right
zk.ui.list({ id, rows: [{ id, title, subtitle, meta, badge, selected }] })  // select → row id
zk.ui.search/field({ id, placeholder, value?, lines?, focus? })  // change (700 ms), submit
zk.ui.chips({ id, chips, selected, select: "none"|"one"|"many" })  button  buttons (≤ 3)
zk.ui.open(screenId, params), close(result), refresh()           // result → screen below
Event: open | tap | select | swipe | change | submit | chips | result | back
run(target, again?) → string | { text, actions: [{ id, title }] ≤ 2 }   // again = { action, text }
```

## What each example shows

- **Feed Riffle** (`feed-riffle/`, moved from the skeleton): four screens. *Deck* (main): card with
  "In library" badge, swipe/buttons for Discard·Skip·Keep, recent collections as `select: "none"`
  chips that file at once, Undo (`cancel`), Done; the summary when the deck runs out. *Keep*: search
  + list of collections (old copy's hoisted, "already in"), tag chips + tag field, note field,
  "Move the old one to the trash" (the app's own confirmation). *Feeds*: list with counts, Fetch
  now. *The sitting*: figures and feeds left. Background refresh → badge "N new" + `refresh()`.
- **Explain with AI**: returns `{ text, actions: [Save as note] }`; the re-run saves `again.text`.
- **Highlights to Markdown**: a main status screen — books, last run, latest files, Export now.

## Clunky spots found, and the fix (in `zk.d.ts`)

1. **No place for set-up or "a sitting starts".** Render is 1 s and runs often; Feed Riffle must
   reset its tally and clear the badge once per opening, and the filing screen loads collections
   and the old copy's notes. **Added an `open` event**, first, before the first render, with
   `onEvent`'s time ("Loading…" meanwhile). `render` may also be async for quick reads.
2. **"Save as note" needed the answer again.** Re-running with only `{ action }` meant caching the
   answer in a module variable or asking the model twice. **`again` carries the pop-up's `text`.**
3. **List taps vs button taps.** One `tap` with an optional `row` needed `!` everywhere. **List rows
   send `select { id, row }`.**
4. **Clearing a field after submit** (the tag box). A pure "initial value" can't clear; a fully
   controlled one clobbers typing. **Rule: `value` omitted → the app keeps the text; given → shown
   whenever it differs from the text last sent in `change`/`submit`.** Plus: a pending `change`
   is always delivered before any other event, so File sees the note just typed.
5. **`message && zk.ui.notice(message)`** yields `""`. **Children skip every falsy value.**
6. **The chips event** carries `chip` (the one tapped) besides `selected`, so `select: "none"`
   chips act as small buttons (recent collections) without a separate block.
7. **`zk.screen` / `zk.Screen` collide** in the generated reference (case-insensitive files): the
   spec type is `ScreenSpec<P>`.

## Decisions I made (please confirm or change)

- `title` is a static string; the deck shows the feed's name in a label row, not the header.
- `render(params)`: `params` is the JSON copy given to `zk.ui.open`, `undefined` from the launcher.
- `zk.ui.open` works from `onEvent` **and from an action's `run`** (an item action may open a
  screen); it takes effect when the call ends. `close` only from `onEvent`. Back closes at once;
  the `back` event comes after, and every child close (Back included) sends `result` below.
- `onEvent` errors show as an action's error pop-up and the screen renders again (not the
  "can't show this screen" state, which is for `render`).
- A toggle is a one-chip `chips` (Feed Riffle's replace switch). Reads fine; no `toggle` block.

## Open questions

- A card body pages with **up/down** swipes, since left/right are verdicts in a deck. Right on PW5?
- Chips toggle optimistically; if the plugin doesn't keep the selection, the next render puts them
  back. Acceptable, or should the app keep chip selection itself?
- **Known break**: the refreshed Highlights to Markdown fixture calls the new `zk.screen(id, {…})`,
  which the shipped C++ binding rejects; `example: Highlights to Markdown…` fails until T3 (or a
  one-line change in `bindings_entry.cpp` accepting the object form).
