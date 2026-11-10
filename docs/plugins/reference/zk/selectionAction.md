[Plugin API v0](../README.md) / [zk](../zk.md) / selectionAction

# Function: selectionAction()

```ts
function selectionAction(action): void;
```

Adds an action to the reader's selection bar (what appears when the user selects text in a
book): the bar gets a **More…** button listing every plugin's selection actions, with the
plugin's name beside each. While `run` runs, the app shows "Working…"; then the result pop-up
shows what it returned (see [ActionResult](ActionResult.md)), or "<Plugin> couldn't do that: <message>"
if it threw. The user can close the pop-up early; the call is then stopped.

`run` has at least 30 seconds, counted from when it starts running; time waiting for the
user's confirmation doesn't count. It is user-caused: it may change Zotero and ask for
removals.

No permission: the selection and its item are handed over because the user chose the action.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `action` | [`Action`](Action.md)\<[`TextSelection`](TextSelection.md)\> |

## Returns

`void`

## Example

```ts
zk.selectionAction({
  id: "explain",
  title: "Explain",
  run: async ({ text, item }) => {
    const answer = await explain(text, item.title);
    return { text: answer, actions: [{ id: "note", title: "Save as note" }] };
  },
});
```
