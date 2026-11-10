[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / ChipsBlock

# Interface: ChipsBlock

See [chips](chips.md).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="chips"></a> `chips` | ( \| `string` \| \{ `id`: `string`; `title`: `string`; \})[] | A string is its own id and title. |
| <a id="id"></a> `id` | `string` | - |
| <a id="kind"></a> `kind` | `"chips"` | - |
| <a id="select"></a> `select?` | `"one"` \| `"many"` \| `"none"` | Default `"many"`. |
| <a id="selected"></a> `selected?` | `string`[] | Ids of the selected chips. |
