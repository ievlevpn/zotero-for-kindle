[Plugin API v0](../README.md) / [zk](../zk.md) / Item

# Interface: Item

A library item (a paper, book, …), the same shape wherever it comes from. Fields Zotero leaves
empty are `null`; lists are empty arrays.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="abstract"></a> `abstract` | `string` \| `null` | - |
| <a id="arxiv"></a> `arxiv` | `string` \| `null` | The arXiv id, without a version suffix (`"2401.01234"`). |
| <a id="citationkey"></a> `citationKey` | `string` \| `null` | The citation key: Zotero's own `citationKey` field when it has one, else the `Citation Key:` line Better BibTeX writes into *Extra*. |
| <a id="collections"></a> `collections` | `string`[] | Keys of the collections it is in. |
| <a id="creators"></a> `creators` | [`Creator`](Creator.md)[] | Authors, editors, … in order. |
| <a id="date"></a> `date` | `string` \| `null` | The date as entered in Zotero (free-form, e.g. `"2024-01-03"` or `"March 2019"`). |
| <a id="doi"></a> `doi` | `string` \| `null` | The DOI, without a `https://doi.org/` prefix. |
| <a id="extra"></a> `extra` | `string` \| `null` | Zotero's *Extra* field. |
| <a id="isbn"></a> `isbn` | `string` \| `null` | - |
| <a id="key"></a> `key` | `string` | Zotero's key, e.g. `"ABCD2345"`. |
| <a id="publication"></a> `publication` | `string` \| `null` | Journal, book, proceedings or website title. |
| <a id="read"></a> `read` | `boolean` | Whether it is marked read. |
| <a id="tags"></a> `tags` | `string`[] | Tag names. |
| <a id="title"></a> `title` | `string` | The title; `""` if it has none. |
| <a id="type"></a> `type` | [`ItemType`](ItemType.md) | Zotero's item type. |
| <a id="url"></a> `url` | `string` \| `null` | - |
| <a id="year"></a> `year` | `number` \| `null` | The year, parsed from `date`. |
