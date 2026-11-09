[Plugin API v0](../../README.md) / [zk](../../zk.md) / [library](../library.md) / findByIdentifier

# Function: findByIdentifier()

```ts
function findByIdentifier(id): Promise<Item[]>;
```

The items with a DOI, arXiv id or ISBN — the way to ask "is this paper already in my
library?". DOIs match case-insensitively and with or without a `https://doi.org/` prefix;
arXiv ids match whatever the version suffix (`2401.01234v3` finds `2401.01234v1`); ISBNs
match as ISBN-10 or ISBN-13. Several identifiers match any of them. Usually zero or one item;
more if the library has duplicates.

**Permission**: `library:read`.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `id` | \{ `arxiv?`: `string`; `doi?`: `string`; `isbn?`: `string`; \} |
| `id.arxiv?` | `string` |
| `id.doi?` | `string` |
| `id.isbn?` | `string` |

## Returns

`Promise`\<[`Item`](../Item.md)[]\>

## Example

```ts
const [existing] = await zk.library.findByIdentifier({ arxiv: "2401.01234v2" });
if (existing) console.log("already have", existing.title);
```
