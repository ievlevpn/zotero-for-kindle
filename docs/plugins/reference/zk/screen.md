[Plugin API v0](../README.md) / [zk](../zk.md) / screen

# Function: screen()

```ts
function screen(id, render): void;
```

Registers a screen of the plugin's own.

**Not shown in this version yet, and provisional.** A later version designs screens and the building blocks
they are made of; `render`'s argument and result will be typed then. In API v0 the screen is
recorded but can't be opened.

## Parameters

| Parameter | Type |
| ------ | ------ |
| `id` | `string` |
| `render` | (`params`) => `unknown` |

## Returns

`void`

## Example

```ts
zk.screen("cards", () => ({}));
```
