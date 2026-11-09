[Plugin API v0](README.md) / clearTimeout

# Function: clearTimeout()

```ts
function clearTimeout(id): void;
```

Cancels a timer set with [setTimeout](setTimeout.md); unknown ids are ignored. No permission.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `id` | `number` \| `undefined` |

## Returns

`void`

## Example

```ts
const timer = setTimeout(() => console.log("slow"), 2000);
await work();
clearTimeout(timer);
```
