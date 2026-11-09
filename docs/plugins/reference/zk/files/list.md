[Plugin API v0](../../README.md) / [zk](../../zk.md) / [files](../files.md) / list

# Function: list()

```ts
function list(dir?): Promise<string[]>;
```

The names of the entries in a folder (default: `data/` itself); folders end with `/`.
Resolves `[]` for a folder that doesn't exist.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `dir?` | `string` |

## Returns

`Promise`\<`string`[]\>

## Example

```ts
const exports = (await zk.files.list("highlights")).filter((n) => n.endsWith(".md"));
```
