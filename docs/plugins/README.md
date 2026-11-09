# Plugins

JavaScript plugins for Zotero for Kindle: what they are, how to use them, and how to write one.

**Using plugins**

- [Using plugins](using.md): installing one, what its permissions mean, turning plugins on and off,
  and what to do if one gets in the way.

**Writing plugins**

- [Getting started](getting-started.md): from an empty folder to a plugin running in the desktop
  build and on your Kindle.
- [Concepts](concepts.md): how plugins run, permissions, writing to Zotero, events, background
  work, limits.
- [Recipes](recipes.md): short complete plugins for common jobs.
- [Debugging and testing](debugging.md): logs, the desktop loop, common errors.
- [API reference](reference/README.md), generated from
  [`sdk/types/zk.d.ts`](../../sdk/types/zk.d.ts).
- [API changelog](api-changelog.md): the plugin API is v0, still unstable.

The SDK is in [`sdk/`](../../sdk/): the types, the [template](../../sdk/template/), and the
example plugins in [`sdk/examples/`](../../sdk/examples/), of which
[Highlights to Markdown](../../sdk/examples/highlights-to-markdown/) is the one to read first.
