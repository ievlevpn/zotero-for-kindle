[Plugin API v0](../README.md) / [zk](../zk.md) / ItemFields

# Interface: ItemFields

The metadata [zk.zotero.createItem](zotero/createItem.md) takes. `type` and `title` are required.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="abstract"></a> `abstract?` | `string` | - |
| <a id="arxiv"></a> `arxiv?` | `string` | The arXiv id (a version suffix is kept as given). |
| <a id="creators"></a> `creators?` | [`Creator`](Creator.md)[] | - |
| <a id="date"></a> `date?` | `string` | Free-form, as in Zotero; ISO (`"2024-01-03"`) is best. |
| <a id="doi"></a> `doi?` | `string` | - |
| <a id="extra"></a> `extra?` | `string` | - |
| <a id="isbn"></a> `isbn?` | `string` | - |
| <a id="publication"></a> `publication?` | `string` | - |
| <a id="title"></a> `title` | `string` | - |
| <a id="type"></a> `type` | [`ItemType`](ItemType.md) | - |
| <a id="url"></a> `url?` | `string` | - |
