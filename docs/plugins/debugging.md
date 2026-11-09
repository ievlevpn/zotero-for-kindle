# Debugging and testing

## Where the log goes

`console.log`, `console.warn` and `console.error` write to the plugin's log, and so do the app's
own notes about the plugin: errors with their stack, limit stops, restarts, removals not asked.

- **The desktop build** with `--plugins-dir` prints every line to its terminal:

  ```
  [plugin hello] 2026-11-09 22:59:41 hello 0.1.0 loaded
  [plugin hello] 2026-11-09 23:04:10 [warn] feed returned no entries
  ```

- **`plugin.log`** in the plugin's folder keeps it all, on the desktop and on the Kindle
  (`zotero/plugins/<id>/plugin.log` on the USB drive). At 256 KB it moves to `plugin.log.1` and a
  new one starts.
- **Settings → Plugins → the plugin** shows its last three errors and, in the list, a
  note when it needs attention.

The app's own log (the desktop terminal, `zotero/log/zotero.log` on the Kindle) has lines such as
`plugins: hello/main.js changed: reloading`.

## The desktop loop

```sh
cd "$ZK"   # the repo's top folder
./build-desktop/zotero --device pw5 --plugins-dir ~/zk-plugins/hello/dist
```

- `--plugins-dir <dir>` uses `<dir>` as the plugins folder: every folder in it that holds a
  `plugin.json` is a plugin. Point it at your plugin's `dist/`, or at a folder of several plugins.
- Logs go to the terminal; the plugins that are on start with the app, and a plugin starts as soon
  as you turn it on.
- At start, the terminal has one line for each plugin that won't run, and why:

  ```
  plugin hello: off — turn it on in Settings → Plugins, or start with --enable-plugin hello
  plugin hello: incompatible — needs app 0.3.0 (this app is 0.2.9); lower minApp in plugin.json to run it here
  plugin hello: can't load — The id "hi" in plugin.json must be the folder's name.
  ```

- `--enable-plugin <id>` (repeatable, only with `--plugins-dir`) turns a plugin on in that folder,
  as **Turn on** in Settings → Plugins does, for your own plugins. It is remembered.
- When a plugin's `main.js` changes (`npm run build`), that plugin is reloaded within a couple of seconds:
  `main.js` runs afresh; its storage and files stay. A folder that appears while the app runs is
  picked up the same way.
- `--device` is `pw5`, `scribe` or `kt4`; add `--landscape` for landscape.
- The plugins folder holds the app's records too, in files whose names start with a dot:
  `.state.json` (which plugins are on, their failures, badges and last background run), `.starts`
  and `.running` and, in each plugin's folder, `storage.db`, `data/`
  and `plugin.log`. Delete `storage.db` and `data/` (with the app closed), or use *Reset its data*
  on the plugin's page, to start from nothing.

### Making events happen

Do the thing in the desktop window, as on the Kindle:

| Event | In the desktop app |
|---|---|
| `bookOpened` | open a downloaded book |
| `bookFinished` | turn past its last page |
| `highlightCreated`, `highlightChanged`, `highlightDeleted` | select text in a book and highlight it; change its colour or comment; delete it |
| `itemMarkedRead`, `itemMarkedUnread` | long-press an item: *Mark as read* / *Mark as unread* |
| `syncFinished`, and events with `origin: "zotero"` | change something in Zotero desktop, then tap the sync button on Home |

The background task runs after a successful sync, at most once an hour. A plugin that never ran
its task is due at the next sync. To run it again sooner, close the app, set the plugin's
`"lastBackgroundMs"` to `0` in `<plugins dir>/.state.json`, start the app and sync.

## Type-checking

`npm run check` type-checks your plugin against `zk.d.ts`: wrong names, missing arguments,
the shapes of events and items. Your editor does the same as you type, and shows each call's
documentation with an example.

A missing permission is not a type error: the app reports it when the call is made, with the
permission to add.

## Testing

Plugins run only inside the app, but most of a plugin's logic doesn't need `zk`. Keep it in plain
functions (formatting, parsing, deciding what to do) in their own files, which `build.mjs` bundles
into `main.js`, and test those with Node's test runner or any other. Keep the calls to `zk` thin,
and try them in the desktop build against your own library.

## Common errors

| What you see | What it means | What to do |
|---|---|---|
| `zk.on("highlightCreated") needs "annotations:read" in plugin.json` | A `zk.PermissionError`: the plugin called something it didn't declare. From `zk.on`, `main.js` stops loading. | Add the permission to `plugin.json` and build. |
| `main.js imports "./util.js", but a plugin is one file: bundle everything into one main.js (the SDK's build does this)` | `main.js` wasn't bundled, or was built without the template's `build.mjs`. | Build with `npm run build`. |
| `ReferenceError: require is not defined` (or `process`, `window`, `document`) | Your code or a bundled library expects Node or a browser. | Use the API and ES2023 instead; pick a library that needs neither. |
| `Stopped: the highlightCreated handler took longer than 5 s` | A `zk.LimitError` (`time`). The call was stopped; what it started was cancelled. | Do less per event: note the work in `zk.storage` and do it in the background task (30 s). Give fetches a `timeout`. |
| `Stopped: … used more than 32 MB of memory` / `… ran out of stack (too much recursion)` | A `zk.LimitError` (`memory`, `stack`). | Read lists lazily (`for await`), keep less in memory, recurse less. |
| `Hello restarted: its code was stopped mid-run, so main.js runs afresh …` | After a stop that cut into running code, the plugin was reloaded; module variables started over. | Keep what must survive in `zk.storage`. |
| `Hello stopped working — turned off after 3 failures in a row` | Three errors or stops in a row. Settings → Plugins says *Turned off — it stopped working*. | Fix the cause (the errors before it are in the log), then turn it on again. |
| `The app stopped while Hello was running — turned off` | The app crashed while the plugin's code ran. | Look at what it did last in `plugin.log`. Turn it on again. |
| `zk.fetch: only https:// URLs are allowed (…)` | Plain `http://` isn't allowed. | Use `https://`. |
| `zk.fetch("…") needs "net:api.example.org" in plugin.json` | The host isn't declared. | Add `net:<host>`; `net:*.example.org` covers its subdomains. |
| `zk.fetch: plugins can't connect to api.zotero.org (zotero.org and the WebDAV server are the app's own)` | Zotero and the user's WebDAV server are never reachable from a plugin. | Read the library with `zk.library`; change it with `zk.zotero`. |
| `… is on your local network; plugins can reach it only if it is declared by its exact name ("net:nas.local") in plugin.json` | `net:*` and `net:*.<domain>` reach only the internet. | Declare the host by its exact name. |
| `LimitError` with `limit` `responseSize`, `responseTime` or `fetches` | A response over 10 MB, a request over 30 s (or its `timeout`), or more than 8 fetches under way. Only that fetch fails. | Ask for less, or await some fetches before starting more. |
| `zk.storage.update can't be called inside another update's function` | A storage write inside an update's function would wait for itself. | Return the new value instead; do other writes after the update. |
| `zk.files: "…" is outside the plugin's data folder` | Paths are relative to `data/`, without `..`. | Use a relative path. |
| `zk.zotero.trash(…): not asked, resolved null — removals are only asked in answer to something the user did, …` | Removals from the background task, a sync's event, a timer or loading resolve `null` without asking. | Ask in answer to a Kindle event (`origin: "kindle"`). |
| `zk.zotero.deleteNote: only a note this plugin added can be deleted (…)` | Other notes can't be deleted by a plugin. | Check `note.createdBy === zk.plugin.id`. |
| `settings.json isn't a JSON object; zk.settings is {}` | The user's `settings.json` didn't parse. | Fix the file; the plugin reads it when it loads. |
| `Uncaught (in promise) …` (a warning) | A promise rejected and nothing awaited it. Not counted as a failure. | Await it, or catch. |
| *Incompatible: needs app 0.3.0* in Settings → Plugins | The app is older than the plugin's `minApp` (the app's version is on the Settings screen, and in the desktop app's first line). | Update the app. Building the app from a pre-release checkout, lower `minApp` in your copy. |
| *Incompatible: written for plugin API 1* | The plugin was written for another API version. | Use a version of the plugin for this app's API (0). |
| *Can't load: The id "…" in plugin.json must be the folder's name.* (or another problem with `plugin.json`) | The folder can't be loaded as it is. | Fix `plugin.json` or the folder's name. `node "$ZK/sdk/validate-manifest.mjs" plugin.json` checks a manifest against the schema (run `npm install` in `$ZK/sdk` once first). |
| *New — not turned on yet*, and nothing in the log | The plugin is off. | Turn it on in Settings → Plugins (or, on the desktop, start with `--enable-plugin <id>`). |
| On, but nothing in the log | On the Kindle, plugins load at their first event (a sync will do). The desktop build with `--plugins-dir` loads a plugin as soon as it is turned on. | Sync. |
