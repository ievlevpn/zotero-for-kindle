# The plugin registry

`registry.json` lists the plugins in the Zotero for Kindle catalog. It is published as the latest
release of `ievlevpn/zotero-for-kindle-plugins`, with `registry.json.sig`: the maintainer signs it
with the app's release key, and the app ignores one that doesn't verify.

## Submitting a plugin

Open a pull request that adds one entry to `plugins`:

```json
{ "id": "feed-riffle", "name": "Feed Riffle", "author": "Pavel Ievlev",
  "description": "Riffle through RSS feeds and keep papers into Zotero.",
  "repo": "ievlevpn/zk-feed-riffle", "publicKey": "<base64 of 32 bytes>" }
```

The plugin's repo needs a latest GitHub release with `plugin.zip` and `plugin.zip.sig`, made with
[`sdk/zk-plugin.mjs`](../sdk/zk-plugin.mjs). The whole path is in
[Publishing a plugin](../docs/plugins/publishing.md). Check your entry:

```sh
node registry/check.mjs registry/registry.json            # the format (what the app enforces)
node registry/check.mjs registry/registry.json --online   # and that each latest release verifies
```

`revoked` holds `{ "id", "reason" }` for withdrawn plugins; the app turns them off.

## Publishing the registry (maintainer)

```sh
tools/registry.sh sign      # checks, stamps "updated", signs into registry/out/
tools/registry.sh publish   # gh release create on ievlevpn/zotero-for-kindle-plugins
```

`sign` uses `local/release-key`, which is never committed.
