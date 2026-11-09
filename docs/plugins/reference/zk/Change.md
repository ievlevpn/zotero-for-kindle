[Plugin API v0](../README.md) / [zk](../zk.md) / Change

# Interface: Change

What a change asked of [zk.zotero](zotero.md) resolves with (a removal: if the user said yes).

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="changeid"></a> `changeId` | `string` | Identifies the change, to [zk.zotero.cancel](zotero/cancel.md) it while it is unsent. |
| <a id="key"></a> `key` | `string` | The key of what the change made or changed: the new item or note; the item filed, tagged, trashed, or taken out of a collection or tag; the note deleted. |
