[Plugin API v0](../README.md) / [zk](../zk.md) / Events

# Interface: Events

Every event [zk.on](on.md) can listen to, and what its handler receives.

## Properties

| Property | Type | Description |
| ------ | ------ | ------ |
| <a id="bookfinished"></a> `bookFinished` | [`BookEvent`](BookEvent.md) | The user turned past the last page of a book or paper. |
| <a id="bookopened"></a> `bookOpened` | [`BookEvent`](BookEvent.md) | A book or paper was opened on the Kindle. |
| <a id="highlightchanged"></a> `highlightChanged` | [`AnnotationsEvent`](AnnotationsEvent.md) | Annotations were changed (comment, colour, text). |
| <a id="highlightcreated"></a> `highlightCreated` | [`AnnotationsEvent`](AnnotationsEvent.md) | Highlights or other annotations were made. |
| <a id="highlightdeleted"></a> `highlightDeleted` | `object` | Annotations were deleted; only their keys are left. |
| `highlightDeleted.annotations` | `Pick`\<[`Annotation`](Annotation.md), `"key"` \| `"attachmentKey"` \| `"itemKey"`\>[] | - |
| `highlightDeleted.origin` | [`Origin`](Origin.md) | - |
| <a id="itemmarkedread"></a> `itemMarkedRead` | [`ItemsEvent`](ItemsEvent.md) | Items were marked read. |
| <a id="itemmarkedunread"></a> `itemMarkedUnread` | [`ItemsEvent`](ItemsEvent.md) | Items were marked unread. |
| <a id="syncfinished"></a> `syncFinished` | `object` | A sync with Zotero finished successfully (fires after the other events of that sync). |
| `syncFinished.initial` | `boolean` | `true` for the first full sync of a library, which fires no other events. |
| `syncFinished.origin` | `"zotero"` | - |
