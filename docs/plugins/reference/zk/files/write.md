[Plugin API v0](../../README.md) / [zk](../../zk.md) / [files](../files.md) / write

# Function: write()

```ts
function write(path, text): Promise<void>;
```

Writes text (UTF-8), replacing the file and creating folders on the way.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `path` | `string` |
| `text` | `string` |

## Returns

`Promise`\<`void`\>

## Example

```ts
await zk.files.write("export.md", "# My highlights\n");
```
