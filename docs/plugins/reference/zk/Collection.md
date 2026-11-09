[Plugin API v0](../README.md) / [zk](../zk.md) / Collection

# Interface: Collection

A collection.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="key"></a> `key` | `string` | - |
| <a id="name"></a> `name` | `string` | - |
| <a id="parent"></a> `parent` | `string` \| `null` | The parent collection's key; `null` at the top level. |
| <a id="path"></a> `path` | `string`[] | Names from the top level down, ending with its own: `["Reading", "arXiv"]`. |
