[Plugin API v0](../README.md) / [zk](../zk.md) / TextSelection

# Interface: TextSelection

What [selectionAction](selectionAction.md) hands to `run`.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="attachment"></a> `attachment` | [`Attachment`](Attachment.md) | The file being read. |
| <a id="item"></a> `item` | [`Item`](Item.md) | The item being read. |
| <a id="pagelabel"></a> `pageLabel` | `string` \| `null` | The page label where the selection starts, if the document has pages. |
| <a id="text"></a> `text` | `string` | The selected text. |
