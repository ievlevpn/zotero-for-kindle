[Plugin API v0](../README.md) / [zk](../zk.md) / Attachment

# Interface: Attachment

A file attached to an item.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="contenttype"></a> `contentType` | `string` | E.g. `"application/pdf"`, `"application/epub+zip"`. |
| <a id="downloaded"></a> `downloaded` | `boolean` | Whether the file is on the Kindle (needed for [zk.library.fullText](library/fullText.md)). |
| <a id="filename"></a> `filename` | `string` \| `null` | - |
| <a id="itemkey"></a> `itemKey` | `string` | The item it belongs to. |
| <a id="key"></a> `key` | `string` | - |
| <a id="title"></a> `title` | `string` | - |
