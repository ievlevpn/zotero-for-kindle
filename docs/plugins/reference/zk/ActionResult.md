[Plugin API v0](../README.md) / [zk](../zk.md) / ActionResult

# Type Alias: ActionResult

```ts
type ActionResult = string | ActionReply | void;
```

What an action's `run` may return: text for the result pop-up (plain text; a blank line
separates paragraphs; paged when long), text with up to two buttons, or nothing (no pop-up).

## Example

```ts
return "Written to adler2007.md";
return { text: answer, actions: [{ id: "note", title: "Save as note" }] };
```
