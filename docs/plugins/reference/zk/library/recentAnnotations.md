[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / recentAnnotations

# Function: recentAnnotations()

```ts
function recentAnnotations(query?): AsyncIterable<Annotation>;
```

Annotations across the library, newest change first, fetched lazily. `since` (an ISO date)
stops at annotations last changed before it.

**Permission**: `annotations:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `query?` | \{ `limit?`: `number`; `since?`: `string`; \} |
| `query.limit?` | `number` |
| `query.since?` | `string` |

## Returns

`AsyncIterable`\<[`Annotation`](../Annotation.md)\>

## Example

```ts
const lastExport = await zk.storage.get<string>("lastExport");
for await (const a of zk.library.recentAnnotations({ since: lastExport })) touched.add(a.itemKey);
```
