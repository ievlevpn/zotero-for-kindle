[Plugin API v0](../README.md) / [zk](../zk.md) / setBadge

# Function: setBadge()

```ts
function setBadge(text): void;
```

Sets a short badge on the plugin, shown wherever the app lists it (e.g. `"42 new"`); `null`
clears it. Kept until changed, across restarts. Long text is cut. No permission.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `text` | `string` \| `null` |

## Returns

`void`

## Example

```ts
zk.setBadge("3 new");
```
