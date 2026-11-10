[Plugin API v0](../../README.md) / [zk](../../zk.md) / [ui](../ui.md) / refresh

# Function: refresh()

```ts
function refresh(): void;
```

Renders the plugin's showing screen again, if one is showing: after work outside `onEvent`
changed what it shows (a background refresh brought new cards). Callable from anywhere;
does nothing when no screen of the plugin is showing.

## Returns

`void`

## Example

```ts
zk.background(async () => { await refreshFeeds(); zk.ui.refresh(); });
```
