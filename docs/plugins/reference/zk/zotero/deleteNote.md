[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / deleteNote

# Function: deleteNote()

```ts
function deleteNote(noteKey): Promise<Change | null>;
```

Asks the user to delete a note — only one this plugin created (see [Note.createdBy](../Note.md#createdby));
any other note is refused. The one hard delete in the API.

**Permission**: `zotero:remove`, and the user's yes each time.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `noteKey` | `string` |

## Returns

`Promise`\<[`Change`](../Change.md) \| `null`\>

## Example

```ts
const mine = (await zk.library.notes(item.key)).filter((n) => n.createdBy === zk.plugin.id);
for (const note of mine) await zk.zotero.deleteNote(note.key);
```
