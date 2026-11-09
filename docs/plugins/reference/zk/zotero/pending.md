[Plugin API v0](../../README.md) / [zk](../../zk.md) / [zotero](../zotero.md) / pending

# Function: pending()

```ts
function pending(): Promise<PendingChange[]>;
```

This plugin's changes not sent to Zotero yet, oldest first.

**Permission**: none.

## Returns

`Promise`\<[`PendingChange`](../PendingChange.md)[]\>

## Example

```ts
const unsent = await zk.zotero.pending();
zk.setBadge(unsent.length ? `${unsent.length} unsent` : null);
```
