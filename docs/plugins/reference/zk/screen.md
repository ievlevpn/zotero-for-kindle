[Plugin API v0](../README.md) / [zk](../zk.md) / screen

# Function: screen()

```ts
function screen<P>(id, screen): void;
```

Registers a screen of the plugin's own. A screen is described, not drawn: `render` returns a
tree of building blocks ([zk.ui.Block](ui/Block.md), made with the [zk.ui](ui.md) helpers or written
as plain objects), and the app draws it in its own style, with its fonts, header, buttons and
page turns. What the user does arrives as events ([zk.ui.Event](ui/Event.md)) at `onEvent`; after
each one the app calls `render` again and redraws only what changed. The plugin keeps all the
state (module variables, [zk.storage](storage-1.md)); the app keeps none.

A screen marked `main` is listed in the app's **Plugins** launcher (the puzzle-piece icon on
Home, shown when some plugin has a main screen; a dot on it when a plugin has a badge, see
[setBadge](setBadge.md)). Other screens are opened from the plugin's own with [zk.ui.open](ui/open.md).

The header shows `title` and the plugin's name, and its back arrow always works: the plugin
is told (a `back` event) but can't keep the screen open. Leaving a screen (Back, the Home
tab) closes it: it renders no more, and an `onEvent` call already running finishes (the
`back` event comes after it), but the [zk.ui.open](ui/open.md), [zk.ui.close](ui/close.md) and
[zk.ui.refresh](ui/refresh.md) it asks for are dropped.

Budgets: `render` 1 s (it is usually milliseconds; it may `await` quick reads such as
[zk.storage](storage-1.md)); `onEvent` at least 30 s, like an action (user-caused: it may change
Zotero and ask for removals; time waiting for the user's answer doesn't count). While
`onEvent` runs the screen stays drawn; taps are queued in order, and after 1 s the app shows
"Working…". A tree over the caps (2,000 blocks, 256 KB of text) or an invalid one, a `render`
that throws or goes over its budget, shows "<Plugin> can't show this screen right now" with
Back, and counts as a failure. An error thrown by `onEvent` is shown as an action's is
("<Plugin> couldn't do that: <message>"), and the screen is rendered again.

No permission.

## Type Parameters

| Type Parameter | Default type | Description |
| ------ | ------ | ------ |
| <a id="screenp"></a> `P` | `undefined` | What the screen is opened with: the `params` of [zk.ui.open](ui/open.md) (a JSON copy), or `undefined` when opened from the launcher or without params. |

## Parameters

| Parameter | Type |
| ------ | ------ |
| `id` | `string` |
| `screen` | [`ScreenSpec`](ScreenSpec.md)\<`P`\> |

## Returns

`void`

## Examples

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

```ts
// a screen opened with params: zk.ui.open("file", { cardId: card.id })
zk.screen<{ cardId: string }>("file", {
  title: "File",
  render: async ({ cardId }) => zk.ui.text((await cardById(cardId))?.title ?? "Gone"),
});
```
