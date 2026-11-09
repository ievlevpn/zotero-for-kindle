[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / cancel

# Function: cancel()

```ts
function cancel(changeId): Promise<boolean>;
```

Withdraws one of this plugin's changes that hasn't been sent to Zotero yet, of any kind — no
confirmation, since nothing leaves Zotero. Withdrawing an add takes it back (withdrawing a
new item also withdraws what was added to it); withdrawing a removal the user agreed to puts
back what it removed (an item comes back out of the trash, a deleted note comes back).
Resolves `true` if withdrawn, `false` if the change was already sent or isn't this plugin's.
Undoing a sent add is a removal: ask with [trash](trash.md) and friends. Undoing a sent removal
isn't possible from a plugin (restoring is an edit); the user restores it in Zotero.

**Permission**: none beyond the one the change needed.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `changeId` | `string` |

## Returns

`Promise`\<`boolean`\>

## Example

```ts
const changeId = await zk.storage.get<string>(`undo:${entry.id}`);
if (changeId && !(await zk.zotero.cancel(changeId))) {
  await zk.zotero.trash(itemKey); // already synced: ask instead
}
```
