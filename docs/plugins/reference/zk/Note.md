[Plugin API v0](../README.md) / [zk](../zk.md) / Note

# Interface: Note

A Zotero note attached to an item.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="created"></a> `created` | `string` | ISO 8601. |
| <a id="createdby"></a> `createdBy` | `string` \| `null` | The id of the plugin that created it, or `null` (made in Zotero). |
| <a id="html"></a> `html` | `string` | The note's HTML, as Zotero stores it. |
| <a id="itemkey"></a> `itemKey` | `string` | The item it belongs to. |
| <a id="key"></a> `key` | `string` | - |
| <a id="modified"></a> `modified` | `string` | ISO 8601. |
