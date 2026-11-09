[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / addNote

# Function: addNote()

```ts
function addNote(itemKey, html): Promise<Change>;
```

Adds a child note to an item. `html` is Zotero note HTML (paragraphs, headings, lists,
links, emphasis). `change.key` is the new note's key.

**Permission**: `zotero:notes`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `itemKey` | `string` |
| `html` | `string` |

## Returns

`Promise`\<[`Change`](../Change.md)\>

## Example

```ts
await zk.zotero.addNote(item.key, `<p>Finished on ${new Date().toDateString()}.</p>`);
```
