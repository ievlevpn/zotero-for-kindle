[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / Event

# Type Alias: Event

```ts
type Event = 
  | {
  kind: "open";
}
  | {
  id: string;
  kind: "tap";
}
  | {
  id: string;
  kind: "select";
  row: string;
}
  | {
  direction: "left" | "right";
  id: string;
  kind: "swipe";
}
  | {
  id: string;
  kind: "change";
  text: string;
}
  | {
  id: string;
  kind: "submit";
  text: string;
}
  | {
  chip: string;
  id: string;
  kind: "chips";
  selected: string[];
}
  | {
  from: string;
  kind: "result";
  value: Json | undefined;
}
  | {
  kind: "back";
};
```

What `onEvent` receives. Events arrive one at a time, in the order they happened; a
pending `change` (typing not yet sent) is always sent before any other event of the same
screen, so a tap on "File" sees the note just typed.

- `open` — the screen is opening; first, before its first render ("Loading…" shows until
  it returns). For set-up slower than `render`'s 1 s.
- `tap` — a [button](button.md), or one of [buttons](buttons.md), by its `id`.
- `select` — a [list](list.md) row: `id` is the list's, `row` the row's.
- `swipe` — the [deck](deck.md)'s card, left or right.
- `change` / `submit` — a [search](search.md) or [field](field.md): its whole text.
- `chips` — a chip tapped: which one, and the selection it makes (keep it and render it as
  `selected`, or the chips go back as they were).
- `result` — a screen this one opened has closed: its id and what [close](close.md) gave
  (`undefined` if closed with Back or without a result).
- `back` — the user left this screen. It is already closed; nothing is rendered after.

## Example

```ts
onEvent: async (e) => {
  if (e.kind === "swipe") await (e.direction === "left" ? discard() : keep());
  else if (e.kind === "tap" && e.id === "undo") message = await undo();
  else if (e.kind === "result" && typeof e.value === "string") message = e.value;
}
```
