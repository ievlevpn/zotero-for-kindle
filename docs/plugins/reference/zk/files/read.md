[Plugin API v0](../../README.md) / [zk](../../zk.md) / [files](../files.md) / read

# Function: read()

```ts
function read(path): Promise<string>;
```

The file's text (UTF-8). Rejects if it doesn't exist.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `path` | `string` |

## Returns

`Promise`\<`string`\>

## Example

```ts
const template = await zk.files.read("template.md");
```
