# Screens and entry points

A plugin can add actions to the reader and the item details page, and screens of its own. Nothing
here needs a permission: the user starting an action or opening a screen is the consent. The exact
types are in the [API reference](reference/README.md); the working example is
[Feed Riffle](../../sdk/examples/feed-riffle/src/main.ts), four screens commented top to bottom.

## Where the user finds a plugin

| Where | What you register | What the user sees |
|---|---|---|
| Reader, after selecting text | `zk.selectionAction` | **More…** in the selection bar lists the actions, each with the plugin's name |
| An item's details page | `zk.itemAction` | the **Plugins** section: a row per action |
| Home | `zk.screen` with `main: true` | the puzzle-piece icon opens the launcher of `main` screens; a dot on the icon when a plugin has a badge ([`zk.setBadge`](reference/zk/setBadge.md)) |

The icon is shown only when some plugin has a `main` screen.

## Actions

```ts
zk.selectionAction({
  id: "explain",
  title: "Explain",
  run: async ({ text, item }) => ({
    text: await explain(text, item.title),
    actions: [{ id: "note", title: "Save as note" }],
  }),
});
```

- While `run` works the app shows **Working…**. Tapping it again stops the run.
- `run` has at least 30 s (time waiting for the user's yes doesn't count) and is user-caused: it
  may change Zotero and ask for removals.
- What `run` returns goes into the **result pop-up**: a string, `{ text, actions }`, or nothing
  (no pop-up). `actions` are at most 2 buttons. Tapping one calls `run` again with the same target
  and `again = { action, text }`; what that call returns replaces the pop-up.
- If `run` throws, the pop-up says "<Plugin> couldn't do that: <message>".

An item action may also `zk.ui.open` one of the plugin's screens (see the [form recipe](recipes.md#a-form)).

## Screens

A screen is described, not drawn. `render` returns blocks; the app draws them in its own style and
redraws only what changed. What the user does comes to `onEvent`, and `render` runs again after
each event. You keep the state in module variables or `zk.storage`; the app keeps none.

```ts
let count = 0;
zk.screen("counter", {
  title: "Counter",
  main: true,
  render: () => zk.ui.column([
    zk.ui.text(`Tapped ${count} times`, { style: "title" }),
    zk.ui.button({ id: "more", title: "Tap", primary: true }),
  ]),
  onEvent: (e) => { if (e.kind === "tap" && e.id === "more") count++; },
});
```

### Blocks

| Block | Helper | Sends |
|---|---|---|
| column, row | `zk.ui.column(children)`, `zk.ui.row(children)` | |
| text (title, body, meta, label) | `zk.ui.text(text, { style, lines })` | |
| card | `zk.ui.card({ title, meta, body, badge })` | |
| deck (one card, swiped) | `zk.ui.deck({ id, card, empty })` | `swipe` |
| list | `zk.ui.list({ id, rows, empty })` | `select` |
| search, field | `zk.ui.search({ id })`, `zk.ui.field({ id, lines })` | `change`, `submit` |
| chips | `zk.ui.chips({ id, chips, selected, select })` | `chips` |
| button, buttons (3 at most) | `zk.ui.button({ id, title, primary })` | `tap` |
| divider, space, notice | `zk.ui.divider()`, `zk.ui.space()`, `zk.ui.notice(text)` | |

Children skip falsy values, so `message && zk.ui.notice(message)` works. A block the user can act
on needs an `id`, unique on its screen. There are no sizes, colours or images; the app lays the
blocks out on every Kindle. Caps: 2,000 blocks, 256 KB of text, 16 levels deep, at most 6
children in a row.

### Events

`open` (first, before the first render), `tap`, `select`, `swipe`, `change`, `submit`, `chips`,
`result` and `back`. Events arrive one at a time, in order; typing not yet sent as `change` is sent
before any other event, so a **Save** tap sees the note just typed. Chips and fields show what the
user did at once; keep the selection or text and render it, or the next render puts it back.

A field's `value`: left out, the app keeps what was typed; given, it is shown whenever it differs
from the text last sent. That is how a box is cleared after `submit`.

### Several screens

```ts
onEvent: (e) => {
  if (e.kind === "tap" && e.id === "keep") zk.ui.open("file", { cardId: top.id });
  if (e.kind === "result") message = String(e.value);   // the screen below hears the result
},
// in "file": zk.ui.close("Kept into Inbox")
```

`zk.ui.open(id, params)` shows another of your screens over this one, with `params` for its
`render` and `onEvent`. It works from `onEvent` and from an action's `run`. `zk.ui.close(result)`
closes the screen whose `onEvent` is running; the screen below gets a `result` event.
`zk.ui.refresh()` redraws the showing screen after work outside `onEvent`, such as the background
task. Inside `render` it does nothing.

### Time

| | Budget |
|---|---|
| `render` | 1 s (it may `await` quick reads like `zk.storage`) |
| `onEvent` | at least 30 s, user-caused: may write to Zotero and ask for removals |

While `onEvent` runs the screen stays drawn and taps queue; after 1 s the app shows **Working…**.
Tapping it again stops the run.

### Leaving, and what goes wrong

- Back always works. The plugin gets a `back` event after the screen has closed; it can't keep the
  screen open, and the event is not user-caused (don't write to Zotero from it).
- Switching to another tab closes the plugin's screens.
- A `render` that throws, goes over 1 s, or returns an invalid or too large tree shows
  "<Plugin> can't show this screen right now" with Back, and counts as a failure.
- An `onEvent` error shows in the result pop-up as "<Plugin> couldn't do that: <message>", and the
  screen renders again.

## Recipes

A [deck](recipes.md#a-deck-of-cards), a [form](recipes.md#a-form) and a
[list with search](recipes.md#a-list-with-search): each a small plugin, type-checked by
`sdk/check.sh`.
