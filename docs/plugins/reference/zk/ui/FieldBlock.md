[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / FieldBlock

# Interface: FieldBlock

See [field](field.md).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="focus"></a> `focus?` | `boolean` | Has the keyboard when the screen opens. |
| <a id="id"></a> `id` | `string` | - |
| <a id="kind"></a> `kind` | `"field"` | - |
| <a id="label"></a> `label?` | `string` | A small label above the input. |
| <a id="lines"></a> `lines?` | `number` | Lines tall; more than 1 makes Enter a new line instead of `submit`. Default 1. |
| <a id="placeholder"></a> `placeholder?` | `string` | - |
| <a id="value"></a> `value?` | `string` | The text to show. Omitted, the app keeps whatever the user typed. Given, the app shows it whenever it differs from the text last sent to the plugin in a `change` or `submit` event — so a plugin that keeps the text can replace it (clear a tag box after `submit`) without ever undoing what is being typed. |
