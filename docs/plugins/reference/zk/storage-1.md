[Plugin API v0](../README.md) / [zk](../zk.md) / storage

# Variable: storage

```ts
const storage: Storage;
```

A private key-value store, kept in the plugin's own folder. Values are JSON (stored as with
`JSON.stringify`); what `get` and `update` hand back is a fresh copy, so changing it changes
nothing until it is stored. Survives app restarts and updates; emptied by Settings → Plugins → *Reset
its data*. At most 1 MB a value and 16 MB in all; a write that would go over, or one made while
the Kindle has less than 50 MB free, rejects with an `Error` that says so. No permission.

## Example

```ts
await zk.storage.set("lastRun", new Date().toISOString());
const seen = await zk.storage.update<string[]>("seen", (seen = []) => [...seen, entry.id]);
```
