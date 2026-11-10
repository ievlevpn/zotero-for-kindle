# Plugin template

A minimal plugin. Copy this folder, change `plugin.json` and `src/main.ts`, then:

```sh
npm install
node build.mjs     # dist/<id>/ is the folder to copy to the Kindle
```

See [Getting started](../../docs/plugins/getting-started.md).

## Publishing

To put the plugin in the in-app catalog, sign it with your own key and release it on GitHub.
`zk-plugin.mjs` is in the SDK (`sdk/` in a checkout of zotero-for-kindle); run it from this folder
with its path:

```sh
node path/to/sdk/zk-plugin.mjs keygen   # once; keep zk-plugin-key.json private, out of git and CI
node path/to/sdk/zk-plugin.mjs pack     # cleans and builds dist/<id>/, checks plugin.json, writes dist/plugin.zip
node path/to/sdk/zk-plugin.mjs sign     # dist/plugin.zip.sig
```

Attach both files to a GitHub release tagged with the version. The first listing is a pull request
to the registry. See [Publishing a plugin](../../docs/plugins/publishing.md).
