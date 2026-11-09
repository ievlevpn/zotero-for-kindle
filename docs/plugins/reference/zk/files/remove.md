[Plugin API v0](../../README.md) / [zk](../../zk.md) / [files](../files.md) / remove

# Function: remove()

```ts
function remove(path): Promise<boolean>;
```

Removes a file, or a folder with everything in it. Resolves `true` if it was there.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `path` | `string` |

## Returns

`Promise`\<`boolean`\>

## Example

```ts
await zk.files.remove("highlights/old.md");
```
