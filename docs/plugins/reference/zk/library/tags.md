[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / tags

# Function: tags()

```ts
function tags(): Promise<Tag[]>;
```

All tags in the library, by name.

**Permission**: `library:read`.

## Returns

`Promise`\<[`Tag`](../Tag.md)[]\>

## Example

```ts
const names = (await zk.library.tags()).map((t) => t.name);
```
