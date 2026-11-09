[Plugin API v0](../README.md) / [zk](../zk.md) / selectionAction

# Function: selectionAction()

```ts
function selectionAction(action): void;
```

Adds an action to the selection bar (what appears when the user selects text in a book).

**Not shown in this version yet.** In API v0 the action is checked and recorded but doesn't appear;
plugins written now keep working when it does.

`run` has at least 30 seconds (the fetch cap), counted from when it starts running; time
waiting for the user's confirmation doesn't count.

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
    return answer; // shown in the result pop-up
  },
});
```
