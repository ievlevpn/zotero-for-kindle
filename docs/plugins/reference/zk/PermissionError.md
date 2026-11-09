[Plugin API v0](../README.md) / [zk](../zk.md) / PermissionError

# Class: PermissionError

Thrown (or rejected with) when the plugin calls something its `plugin.json` doesn't declare.
The message says what to add, e.g.
`zk.fetch("https://api.deepl.com/…") needs "net:api.deepl.com" in plugin.json`.

## Example

```ts
try {
  await zk.library.fullText(pdf.key);
} catch (e) {
  if (e instanceof zk.PermissionError) console.log("missing", e.permission);
  else throw e;
}
```

## Extends

- `Error`

## Properties

| Property | Modifier | Type | Description | Overrides | Inherited from |
| ------ | ------ | ------ | ------ | ------ | ------ |
| <a id="cause"></a> `cause?` | `public` | `unknown` | - | - | `Error.cause` |
| <a id="message"></a> `message` | `public` | `string` | - | - | `Error.message` |
| <a id="name"></a> `name` | `readonly` | `"PermissionError"` | - | `Error.name` | - |
| <a id="permission"></a> `permission` | `readonly` | [`Permission`](Permission.md) | The permission that was missing, exactly as it must appear in `plugin.json`. | - | - |
| <a id="stack"></a> `stack?` | `public` | `string` | - | - | `Error.stack` |
