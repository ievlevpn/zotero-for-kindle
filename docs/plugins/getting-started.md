# Getting started

From an empty folder to a plugin running in the desktop build, then on your Kindle. The plugin
is the SDK's template, *Hello*: it says when it has loaded, and logs each book you finish.

## What you need

- **This repo, cloned, with the desktop app built** (macOS). See *Building from source* in the
  [README](../../README.md#building-from-source); you should be able to run
  `./build-desktop/zotero --device pw5` from the clone's top folder.
- **Node.js 20 or later**, with npm.
- **Your Zotero account in the desktop app.** On its first start it asks for your user ID and API
  key, as on the Kindle (see [INSTALL.md](../../INSTALL.md#3-start-it-and-sign-in)). Or put them
  in `local/credentials.env` in the clone before you start it:

  ```sh
  ZK_ZOTERO_USER_ID=1234567
  ZK_ZOTERO_API_KEY=your-api-key
  ```

- For the last part: a Kindle with Zotero for Kindle installed, and a USB cable.

In the commands below, `ZK` is the folder you cloned the repo into. Set it in each terminal you
use (change the path to yours):

```sh
ZK=~/zotero-for-kindle
```

## 1. Copy the template

Make a folder for the plugin and copy the template's files into it:

```sh
mkdir -p ~/zk-plugins/hello
cd ~/zk-plugins/hello
cp -R "$ZK"/sdk/template/{plugin.json,package.json,build.mjs,src} .
```

Then give it a `tsconfig.json` that finds the plugin API's types in your clone
(`sdk/types/zk.d.ts`, through the SDK's `sdk/tsconfig.json`):

```sh
cat > tsconfig.json <<EOF
{
  "extends": "$ZK/sdk/tsconfig.json",
  "include": ["src"]
}
EOF
```

You now have:

| File | What it is |
|---|---|
| `plugin.json` | The manifest: the plugin's id, name, version, and the permissions it asks for. |
| `src/main.ts` | The plugin. Read it: it is short and commented. |
| `build.mjs` | Bundles `src/main.ts` (and anything it imports) into one `main.js` with esbuild. |
| `package.json` | esbuild and TypeScript, and the `build` and `check` scripts. |
| `tsconfig.json` | Type-checks `src/` against the plugin API. |

## 2. Install and build

```sh
npm install
npm run build
npm run check
```

`npm run build` prints `built …/zk-plugins/hello/dist/hello/`. That folder, `dist/hello/`, is the
plugin as the app sees it: `plugin.json` and `main.js`. `npm run check` type-checks; it prints
nothing after its `tsc -p .` line when all is well.

`npm install` may warn that install scripts weren't run (`npm warn install-scripts`). That is
harmless here: esbuild works without them.

## 3. Run it in the desktop build

Open a second terminal (keep the first for building) and start the app from the clone's top folder
(it keeps its library in `local/` there):

```sh
ZK=~/zotero-for-kindle
cd "$ZK"
./build-desktop/zotero --device pw5 --plugins-dir ~/zk-plugins/hello/dist
```

`--plugins-dir` is the folder that holds plugin folders; `dist/` holds `hello/`. With it, the app
prints every plugin's log to this terminal, reloads a plugin when its `main.js` changes, and starts
the plugins that are on as soon as it starts.

The first lines in the terminal say which app version runs, and what each plugin that won't run
needs:

```
… INFO zotero 0.3.0 starting (backend=desktop)
plugin hello: off — turn it on in Settings → Plugins, or start with --enable-plugin hello
```

**Check the version.** If it says `plugin hello: incompatible — needs app 0.3.0 (this app is
0.2.9)`, your app is older than the `minApp` in `plugin.json`. That happens when you build the app
from a pre-release checkout: lower `minApp` in your copy to the app's version (here
`"minApp": "0.2.9"`), run `npm run build`, and start the app again.

## 4. Turn it on

A plugin is off until you have read what it may do and turned it on — on the desktop too.

1. On Home, tap the gear at the top right: **Settings**. Tap **Plugins**.
2. The list shows **Hello**, with *New — not turned on yet*. Tap it. (If it says *Incompatible:
   needs app 0.3.0*, the app is older than the plugin's `minApp`: see *Check the version* above.)
3. Its page lists what it can do: *Reads your library*. Tap **Turn on**.

Or, as the plugin's author, turn it on from the command line: quit the app and start it with
`--enable-plugin <id>` (repeat it for several plugins). It does what **Turn on** does, without
the page, so use it only for your own plugins:

```sh
./build-desktop/zotero --device pw5 --plugins-dir ~/zk-plugins/hello/dist --enable-plugin hello
```

In the desktop build a plugin starts as soon as it is turned on. The app's terminal shows:

```
[plugin hello] 2026-11-09 22:59:41 hello 0.1.0 loaded
```

The app remembers that it is on (in `dist/.state.json`), so from now on Hello starts with the app,
with or without `--enable-plugin`. Files in `dist/` whose names start with a dot (`.state.json`,
`.starts`) are the app's own records.

## 5. Edit it and watch it reload

Add a handler for the end of each sync to `src/main.ts`, and build:

```sh
cat >> src/main.ts <<'EOF'

// syncFinished fires after each sync with Zotero. It needs no permission.
zk.on("syncFinished", ({ initial }) => {
  console.log(initial ? "first full sync finished" : "synced");
});
EOF
npm run build
```

Within a couple of seconds the app notices the new `main.js` and reloads Hello: the terminal shows
`plugins: hello/main.js changed: reloading`, then `hello 0.1.0 loaded` again. Now tap the sync button on Home (the round
arrows at the top right). When the sync ends, the terminal shows:

```
[plugin hello] 2026-11-09 23:01:12 synced
```

That is the whole loop: edit, `npm run build`, watch the terminal. To see the template's own
handler, open a book with a few pages and turn past the last one: `Finished "…" by …`.

Everything the plugin logs is also kept in `dist/hello/plugin.log`; storage and files it writes go
to `dist/hello/storage.db` and `dist/hello/data/`. If it throws, the error and its stack show in
the terminal and under *Last errors* on its page; see [Debugging and testing](debugging.md).

## 6. On your Kindle

1. On the Kindle, close Zotero (the × at the top right of Home). The Kindle needs Zotero for
   Kindle 0.3.0 or later, installed and run once (that makes its `zotero/` folder).
2. Connect the Kindle by USB. It shows up as a drive, or, for newer Kindles on a Mac, open it in
   [OpenMTP](https://openmtp.ganeshrvel.com) (as in [INSTALL.md](../../INSTALL.md#2-copy-them-onto-the-kindle)).
3. On the Kindle, make the folder `zotero/plugins/hello/` and copy `plugin.json` and `main.js` from
   `~/zk-plugins/hello/dist/hello/` into it. Copy only those two: the other files there are what the
   desktop run kept. With the Kindle as a drive on a Mac:

   ```sh
   mkdir -p /Volumes/Kindle/zotero/plugins/hello
   cp ~/zk-plugins/hello/dist/hello/{plugin.json,main.js} /Volumes/Kindle/zotero/plugins/hello/
   ```

   If `/Volumes/Kindle/zotero` doesn't exist, Zotero for Kindle isn't installed there: install it
   ([INSTALL.md](../../INSTALL.md)) and start it once first. Over SSH (USBNetwork) the folder is
   `/mnt/us/zotero/plugins/hello/`.
4. Eject the Kindle and start Zotero.
5. **Settings → Plugins → Hello → Turn on**, as on the desktop.

On the Kindle a plugin starts at its first event; a sync will do (tap the sync button on Home).
Its log is `zotero/plugins/hello/plugin.log`, which you can read over USB; its errors also show on
its page in Settings → Plugins. Finish a book and the log says so.

To update it, build, then copy `main.js` (and `plugin.json`, if it changed) over the old one and
restart Zotero. Its storage and data stay.

## 7. Make it yours

Edit `plugin.json`:

```json
{
  "id": "hello",
  "name": "Hello",
  "version": "0.1.0",
  "api": 0,
  "minApp": "0.3.0",
  "author": "Your Name",
  "description": "Logs each book you finish on the Kindle.",
  "main": "main.js",
  "permissions": ["library:read"]
}
```

- `id`: lowercase letters, digits and dashes. The plugin's folder must have this name, so the build
  writes `dist/<id>/`. A new id is a new plugin: turn it on again.
- `name` (up to 40 characters), `description` and `author` are what people see in Settings → Plugins.
- `api`: the plugin API it was written for; `0` is the current one, still unstable.
- `minApp`: the oldest app version it runs on. Plugins arrived in 0.3.0. It must not be newer than
  the app you run (the version line on the Settings screen, *Version 0.3.0 · …*, or the first line
  the desktop app prints); if you build the app from a pre-release checkout, lower `minApp` in your copy.
- `permissions`: what it may do, shown before it is turned on. Ask for what you use and nothing
  more; calling something you didn't declare fails with an error naming the permission to add.
  See [Concepts](concepts.md#permissions).

The `name` in `package.json` is only for npm.

## Sharing a plugin

There is no catalog or store for plugins yet. To share one, share its folder: `plugin.json` and
`main.js` (from `dist/<id>/`), plus a short README saying what it does, which permissions it asks
for and why, and what goes in `settings.json` if it has settings. Others copy the folder to
`/mnt/us/zotero/plugins/<id>/` (`zotero/plugins/<id>/` on the USB drive) and turn it on in
Settings → Plugins after reading its permissions, as in [Using plugins](using.md). Don't share
`plugin.log`, `storage.db` or `data/`: they are your own run's.

An in-app catalog, with signed installs and updates, is planned.

## Next

- [Concepts](concepts.md): how plugins run, permissions, writing to Zotero, events, background
  work, limits.
- [Screens and entry points](screens.md): actions in the reader and on items, screens of your own.
- [Recipes](recipes.md): short complete plugins for common jobs.
- [The API reference](reference/README.md), generated from `sdk/types/zk.d.ts`. Your editor shows
  the same text, with examples, as you type `zk.`.
- The example plugin, [Highlights to Markdown](../../sdk/examples/highlights-to-markdown/src/main.ts):
  a complete plugin, commented as a walk-through.
