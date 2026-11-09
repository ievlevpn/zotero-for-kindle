[Plugin API v0](../README.md) / [zk](../zk.md) / Creator

# Interface: Creator

A creator of an item: a person (`first`, `last`) or an organisation (`name`).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="first"></a> `first?` | `string` | - |
| <a id="last"></a> `last?` | `string` | - |
| <a id="name"></a> `name?` | `string` | A single-field name, for organisations or names that don't split. |
| <a id="type"></a> `type` | `string` & `object` \| `"author"` \| `"editor"` \| `"contributor"` \| `"translator"` | `"author"`, `"editor"`, … |
