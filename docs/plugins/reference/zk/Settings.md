[Plugin API v0](../README.md) / [zk](../zk.md) / Settings

# Interface: Settings

The shape of `settings.json`. Untyped by default; a plugin describes its own by merging fields
into this interface.

## Example

```ts
// main.ts
declare global {
  namespace zk {
    interface Settings { apiKey?: string; language?: string }
  }
}
const lang = zk.settings.language ?? "English"; // string
```

## Indexable

```ts
[key: string]: Json | undefined
```
