[Plugin API v0](../README.md) / [zk](../zk.md) / itemAction

# Function: itemAction()

```ts
function itemAction(action): void;
```

Adds an action to an item's *More…* menu.

**Not shown in this version yet.** In API v0 the action is checked and recorded but doesn't appear.

`run` has at least 30 seconds (the fetch cap), counted from when it starts running; time
waiting for the user's confirmation doesn't count.

No permission: the item is handed over because the user chose the action.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `action` | [`Action`](Action.md)\<[`Item`](Item.md)\> |

## Returns

`void`

## Example

```ts
zk.itemAction({
  id: "export",
  title: "Export highlights",
  run: async (item) => `Written to ${await exportHighlights(item)}`,
});
```
