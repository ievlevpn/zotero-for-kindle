[Plugin API v0](../README.md) / [zk](../zk.md) / Action

# Interface: Action\<T\>

An action the user starts ([selectionAction](selectionAction.md), [itemAction](itemAction.md)).

## Type Parameters

| Type Parameter |
| ------ |
| <a id="t"></a> `T` |

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="id"></a> `id` | `string` | Unique within the plugin: lowercase letters, digits and dashes. |
| <a id="title"></a> `title` | `string` | What the user sees, short (one or two words). |

## Methods

### run()

```ts
run(target): 
  | ActionResult
| Promise<ActionResult>;
```

Does the work. A returned string is shown to the user in the result pop-up (part 2);
a thrown error is shown as the plugin having failed.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `target` | `T` |

#### Returns

  \| [`ActionResult`](ActionResult.md)
  \| `Promise`\<[`ActionResult`](ActionResult.md)\>
