[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / collections

# Function: collections()

```ts
function collections(): Promise<Collection[]>;
```

All collections, each with its full path: My Library's first, then each group library's
(see [libraries](libraries.md)), each sorted by path.

**Permission**: `library:read`.

## Returns

`Promise`\<[`Collection`](../Collection.md)[]\>

## Example

```ts
const inbox = (await zk.library.collections()).find((c) => c.libraryId === 0 && c.path.join("/") === "Reading/Inbox");
```
