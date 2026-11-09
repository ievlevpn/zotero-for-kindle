[Plugin API v0](../README.md) / [zk](../zk.md) / settings

# Variable: settings

```ts
const settings: Readonly<Settings>;
```

The user's settings for this plugin: the parsed `settings.json` next to `plugin.json`, or `{}`
if there is none. Read-only; the user edits the file over USB. Read when the plugin loads.
No permission.

To type your settings, add their fields to [zk.Settings](Settings.md).

## Example

```ts
const apiKey = zk.settings.apiKey;
if (typeof apiKey !== "string") throw new Error('Put your key in settings.json: { "apiKey": "…" }');
```
