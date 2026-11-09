[Plugin API v0](../README.md) / [zk](../zk.md) / plugin

# Variable: plugin

```ts
const plugin: object;
```

This plugin, as installed. No permission.

## Type Declaration

| Name | Type | Description |
| ------ | ------ | ------ |
| <a id="datadir"></a> `dataDir` | `string` | Where its `data/` folder is on the device (e.g. `/mnt/us/zotero/plugins/<id>/data`). For telling the user where an export went; files are read and written with [zk.files](files.md). |
| <a id="id"></a> `id` | `string` | The plugin's id from `plugin.json`; also its folder name. |
| <a id="version"></a> `version` | `string` | The plugin's version from `plugin.json`. |

## Example

```ts
console.log(`${zk.plugin.id} ${zk.plugin.version}, data in ${zk.plugin.dataDir}`);
```
