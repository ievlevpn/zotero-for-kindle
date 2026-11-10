[Plugin API v0](../README.md) / [zk](../zk.md) / itemAction

# Function: itemAction()

```ts
function itemAction(action): void;
```

Adds an action to an item's details page (the long-press sheet's *More…*), in its
**Plugins** section: a row with the action's title and the plugin's name. Runs and shows its
result as [selectionAction](selectionAction.md) does.

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
