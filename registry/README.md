# The plugin registry

`registry.json` lists the plugins in the Zotero for Kindle catalog. Its home is the registry repo,
[`ievlevpn/zotero-for-kindle-plugins`](https://github.com/ievlevpn/zotero-for-kindle-plugins): the
file lives there, pull requests go there, and its latest release carries `registry.json` with
`registry.json.sig`. The maintainer signs it with the app's release key (from this repo's tools),
and the app ignores one that doesn't verify. This folder holds the checker and a copy of the file
that the maintainer signs from.

## Submitting a plugin

Open a pull request on `ievlevpn/zotero-for-kindle-plugins` (not on this repo) that adds one entry
to `plugins` in its `registry.json`:

```json
{ "id": "feed-riffle", "name": "Feed Riffle", "author": "Pavel Ievlev",
  "description": "Riffle through RSS feeds and keep papers into Zotero.",
  "repo": "ievlevpn/zk-feed-riffle", "publicKey": "<base64 of 32 bytes>" }
```

The plugin's repo needs a latest GitHub release with `plugin.zip` and `plugin.zip.sig`, made with
[`sdk/zk-plugin.mjs`](https://github.com/ievlevpn/zotero-for-kindle/blob/master/sdk/zk-plugin.mjs). The whole path is in
[Publishing a plugin](https://github.com/ievlevpn/zotero-for-kindle/blob/master/docs/plugins/publishing.md). Check your entry:

```sh
# in a zotero-for-kindle checkout (check.mjs needs its sdk/), on the registry repo's file:
node registry/check.mjs path/to/zotero-for-kindle-plugins/registry.json            # the format
node registry/check.mjs path/to/zotero-for-kindle-plugins/registry.json --online   # and each release verifies
```

`revoked` holds `{ "id", "reason" }` for withdrawn plugins; the app turns them off.

## Publishing the registry (maintainer)

Once a pull request is merged in the registry repo, copy its `registry.json` over
`registry/registry.json` here, then:

```sh
tools/registry.sh sign      # checks, stamps "updated", signs into registry/out/
tools/registry.sh publish   # gh release create on ievlevpn/zotero-for-kindle-plugins
```

`sign` uses `local/release-key`, which is never committed.
