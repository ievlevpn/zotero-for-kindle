[Plugin API v0](../README.md) / [zk](../zk.md) / ScreenSpec

# Interface: ScreenSpec\<P\>

A screen, as [zk.screen](screen.md) registers it.

## Type Parameters

| Type Parameter | Default type |
| ------ | ------ |
| <a id="p"></a> `P` | `undefined` |

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="main"></a> `main?` | `boolean` | List this screen in the Plugins launcher (opened there with no params). Default `false`. |
| <a id="title"></a> `title` | `string` | Shown in the header (with the plugin's name), and in the launcher for a main screen. Short. |

## Methods

### onEvent()?

```ts
onEvent?(event, params): void | Promise<void>;
```

Handles what the user did ([zk.ui.Event](ui/Event.md)). After it returns, the screen is rendered
again. Budget at least 30 s. Optional: a screen that only shows things needs none.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `event` | [`Event`](ui/Event.md) |
| `params` | `P` |

#### Returns

`void` \| `Promise`\<`void`\>

***

### render()

```ts
render(params): Block | Promise<Block>;
```

The screen as it should look now: one block, usually a [zk.ui.column](ui/column.md). Called when the
screen opens (after the `open` event), after every event, and on [zk.ui.refresh](ui/refresh.md).
Budget 1 s. The app compares the new tree with the last one and redraws only the top-level
children of the root that changed.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `params` | `P` |

#### Returns

[`Block`](ui/Block.md) \| `Promise`\<[`Block`](ui/Block.md)\>
