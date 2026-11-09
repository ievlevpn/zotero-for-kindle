[Plugin API v0](../README.md) / [zk](../zk.md) / Permission

# Type Alias: Permission

```ts
type Permission = 
  | "library:read"
  | "annotations:read"
  | "fulltext:read"
  | "notes:read"
  | `net:${string}`
  | "zotero:create-items"
  | "zotero:collections"
  | "zotero:tags"
  | "zotero:notes"
  | "zotero:remove"
  | "background";
```

A permission, as written in `plugin.json`.
