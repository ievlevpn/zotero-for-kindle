[Plugin API v0](../README.md) / [zk](../zk.md) / PendingChange

# Type Alias: PendingChange

```ts
type PendingChange = object & (
  | {
  collections: string[];
  fields: ItemFields;
  key: string;
  kind: "createItem";
  tags: string[];
}
  | {
  collection: string;
  key: string;
  kind: "addToCollection";
}
  | {
  key: string;
  kind: "addTag";
  tag: string;
}
  | {
  itemKey: string;
  key: string;
  kind: "addNote";
}
  | {
  key: string;
  kind: "trash";
}
  | {
  collection: string;
  key: string;
  kind: "removeFromCollection";
}
  | {
  key: string;
  kind: "removeTag";
  tag: string;
}
  | {
  key: string;
  kind: "deleteNote";
});
```

A change of this plugin's that hasn't been sent to Zotero yet ([zk.zotero.pending](zotero/pending.md)).

## Type Declaration

| Name | Type | Description |
| ------ | ------ | ------ |
| `changeId` | `string` | As returned by the call that asked for it; for [zk.zotero.cancel](zotero/cancel.md). |
| `requested` | `string` | When it was asked for (ISO 8601). |
