[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / collections

# Function: collections()

```ts
function collections(): Promise<Collection[]>;
```

All collections, each with its full path, sorted by path.

**Permission**: `library:read`.

## Returns

`Promise`\<[`Collection`](../Collection.md)[]\>

## Example

```ts
const inbox = (await zk.library.collections()).find((c) => c.path.join("/") === "Reading/Inbox");
```
