[Plugin API v0](../README.md) / [zk](../zk.md) / files

# files

Text files in the plugin's own `data/` folder, for exports the user copies off over USB.
Paths are relative to `data/` and use `/`; a path that leaves the folder (`..`, absolute
paths) is refused. At most 16 MB a file and 64 MB for all of `data/`; a write that would go
over, or one made while the Kindle has less than 50 MB free, rejects with an `Error` that says
so (remove something first). No permission.

## Example

```ts
await zk.files.write("highlights/Gaussian random fields.md", markdown);
console.log("written to", `${zk.plugin.dataDir}/highlights/`);
```

## Functions

- [list](files/list.md)
- [read](files/read.md)
- [remove](files/remove.md)
- [write](files/write.md)
