[Plugin API v0](../README.md) / [zk](../zk.md) / LibraryInfo

# Interface: LibraryInfo

A library on the Kindle: My Library or a Zotero group library ([zk.library.libraries](library/libraries.md)).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="editable"></a> `editable` | `boolean` | Whether changes there can go to Zotero (My Library always; a group when the user may edit it). |
| <a id="id"></a> `id` | `number` | 0 for My Library, else the Zotero group's id. |
| <a id="items"></a> `items` | `number` | How many top-level items it has on the Kindle. |
| <a id="name"></a> `name` | `string` | `"My Library"`, or the group's name. |
