[Plugin API v0](../README.md) / [zk](../zk.md) / Annotation

# Interface: Annotation

An annotation in a book or paper.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="attachmentkey"></a> `attachmentKey` | `string` | The file it is in. |
| <a id="color"></a> `color` | `string` | As in Zotero, e.g. `"#ffd400"`. |
| <a id="comment"></a> `comment` | `string` \| `null` | The user's comment. |
| <a id="created"></a> `created` | `string` | ISO 8601. |
| <a id="itemkey"></a> `itemKey` | `string` | The item that file belongs to. |
| <a id="key"></a> `key` | `string` | - |
| <a id="modified"></a> `modified` | `string` | ISO 8601. |
| <a id="pagelabel"></a> `pageLabel` | `string` \| `null` | The page label shown in the document, e.g. `"12"` or `"xii"`. |
| <a id="tags"></a> `tags` | `string`[] | - |
| <a id="text"></a> `text` | `string` \| `null` | The highlighted text (`null` for notes and images). |
| <a id="type"></a> `type` | `"highlight"` \| `"underline"` \| `"note"` \| `"image"` \| `"ink"` | - |
