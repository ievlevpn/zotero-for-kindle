[Plugin API v0](README.md) / console

# Variable: console

```ts
console: object;
```

Writes to the plugin's log (Settings → Plugins → the plugin, and the terminal of the desktop
build). Arguments are joined with spaces; objects are shown as JSON. `warn` and `error` are
marked as such in the log. The one way to log: standard, with levels, and what bundled
libraries call. No permission.

## Type Declaration

| Name | Type |
| ------ | ------ |
| `error()` | (...`data`) => `void` |
| `log()` | (...`data`) => `void` |
| `warn()` | (...`data`) => `void` |

## Example

```ts
console.log("filed", change.key, "into", collection.path.join(" / "));
console.warn("feed returned no entries", url);
```
