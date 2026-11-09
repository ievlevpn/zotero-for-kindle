[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / trash

# Function: trash()

```ts
function trash(itemKey): Promise<Change | null>;
```

Asks the user to move an item to Zotero's trash (recoverable there). Resolves with the
change if they said yes (`change.key` is the item's key), `null` if not. To trash several,
ask for them together: one confirmation lists them all.

**Permission**: `zotero:remove`, and the user's yes each time.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |

## Returns

`Promise`\<[`Change`](../Change.md) \| `null`\>

## Examples

```ts
// replace an old version with a new one, keeping its notes
for (const note of await zk.library.notes(old.key)) await zk.zotero.addNote(fresh.key, note.html);
if (!(await zk.zotero.trash(old.key))) console.log("kept the old version");
```

```ts
// several at once, in one confirmation
const trashed = await Promise.all(duplicates.map((d) => zk.zotero.trash(d.key)));
```
