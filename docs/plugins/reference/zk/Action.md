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
run(target, again?): 
  | ActionResult
| Promise<ActionResult>;
```

Does the work, and returns what the result pop-up shows (see [ActionResult](ActionResult.md)); a
thrown error is shown as "<Plugin> couldn't do that: <message>".

When the user taps one of the pop-up's buttons (from [ActionReply.actions](ActionReply.md#actions)), `run` is
called again with the same `target` and `again`: which button, and the text the pop-up was
showing. What that call returns replaces the pop-up's content; returning nothing closes it.

#### Parameters

| Parameter | Type |
| ------ | ------ |
| `target` | `T` |
| `again?` | [`ActionAgain`](ActionAgain.md) |

#### Returns

  \| [`ActionResult`](ActionResult.md)
  \| `Promise`\<[`ActionResult`](ActionResult.md)\>

#### Example

```ts
run: async ({ item }, again) => {
  if (again?.action === "note") {
    await zk.zotero.addNote(item.key, `<p>${again.text}</p>`);
    return "Saved as a note.";
  }
  return { text: await explain(item), actions: [{ id: "note", title: "Save as note" }] };
}
```
