[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / notes

# Function: notes()

```ts
function notes(itemKey): Promise<Note[]>;
```

An item's child notes (Zotero note items), as HTML, oldest first.

**Permission**: `notes:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |

## Returns

`Promise`\<[`Note`](../Note.md)[]\>

## Example

```ts
for (const note of await zk.library.notes(old.key)) await zk.zotero.addNote(replacement.key, note.html);
```
