[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / libraries

# Function: libraries()

```ts
function libraries(): Promise<LibraryInfo[]>;
```

The libraries on the Kindle: My Library (`id` 0) first, then the user's Zotero group
libraries that sync (switched on in Settings → Group libraries, and still available), by
name. Items, collections and annotations carry their library's `libraryId` and
`libraryName`. Needs app 0.8.0.

**Permission**: `library:read`.

## Returns

`Promise`\<[`LibraryInfo`](../LibraryInfo.md)[]\>

## Example

```ts
for (const lib of await zk.library.libraries())
  if (!lib.editable) console.log(`${lib.name}: read-only, changes there are refused`);
```
