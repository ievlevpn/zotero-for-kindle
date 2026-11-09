[Plugin API v0](../README.md) / [zk](../zk.md) / BookEvent

# Interface: BookEvent

`bookOpened`, `bookFinished`: these only happen on the Kindle.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="attachment"></a> `attachment` | [`Attachment`](Attachment.md) | The file that was read. |
| <a id="item"></a> `item` | [`Item`](Item.md) | - |
| <a id="origin"></a> `origin` | `"kindle"` | - |
