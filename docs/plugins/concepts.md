# Concepts

How plugins run, what they may do, and what happens when something goes wrong. The details of
every call are in the [API reference](reference/README.md).

## What a plugin is

A folder with a manifest and one JavaScript file:

```
/mnt/us/zotero/plugins/<id>/     (zotero/plugins/<id>/ on the USB drive)
  plugin.json      the manifest
  main.js          the plugin: one ES module, everything bundled into it
  settings.json    the user's settings for it (optional; edited over USB)
  data/            its files (zk.files), made by the app
  storage.db       its key-value store (zk.storage), made by the app
  plugin.log       its log, made by the app (plugin.log.1 is the one before)
```

The plugins folder is outside the app's own folders: app updates and rollbacks never touch it.

You write TypeScript (or JavaScript) and bundle it with esbuild into `main.js`; the template's
`build.mjs` does this, npm packages included. The app runs it with
[QuickJS-ng](https://github.com/quickjs-ng/quickjs): standard ES2023, but neither Node nor a
browser. There is no `require`, `process`, `window` or DOM, and `main.js` can't import other
files. Besides ES2023 you have the one global `zk`, `console` (`log`, `warn`, `error`),
`setTimeout`/`clearTimeout`, `URL`, `URLSearchParams`, `TextEncoder` and `TextDecoder`. A bundled
library works if it needs nothing else.

A plugin never draws, never touches files outside its own folder, never talks to Zotero or WebDAV
itself, and never sees the Zotero API key or WebDAV password. It asks the app, and the app decides.

### plugin.json

```json
{
  "id": "highlights-to-markdown",
  "name": "Highlights to Markdown",
  "version": "0.1.0",
  "api": 0,
  "minApp": "0.3.0",
  "author": "Zotero for Kindle",
  "description": "Keeps a Markdown file of each book's highlights and comments in the plugin's data folder, to copy off over USB.",
  "main": "main.js",
  "permissions": ["library:read", "annotations:read", "background"]
}
```

`id` is lowercase letters, digits and dashes, at most 64, and must be the folder's name. `api` is
the plugin API version it was written for (`0`). `minApp` is the oldest app version it runs on.
Required: `id`, `name`, `version`, `api`, `main`, `permissions`. The schema is
[`sdk/types/plugin-manifest.schema.json`](../../sdk/types/plugin-manifest.schema.json); the SDK's
`sdk/validate-manifest.mjs` checks a file against it.

A folder whose `plugin.json` is broken is listed in Settings → Plugins with what is wrong
(*Can't load: …*). One written for another plugin API, or for a newer app, is listed as
*Incompatible* and not loaded.

## How a plugin runs

- **Off until turned on.** A plugin someone copied in is off until they open its page in
  Settings → Plugins, read its permissions and tap **Turn on**.
- **Loaded when needed.** Plugins load after the app's first screen is shown, at the first event
  or background run. (In the desktop build with `--plugins-dir`, at once.)
- **`main.js` runs once.** Its top level registers what the plugin reacts to — `zk.on(…)`,
  `zk.background(…)` — and the module stays loaded while the plugin is on. Module-level variables
  last from one call to the next, but not across a restart of the app or of the plugin: keep what
  must last in `zk.storage`.
- **`zk.settings`** is the user's `settings.json`, read when the plugin loads. After an edit, the
  plugin sees the new settings once it loads again: turn it off and on, or restart the app.

### One thread, so everything is async

All plugins run on one plugin thread, apart from the screen and the sync. The screen never waits
for a plugin: events are queued to the plugin thread, and anything that touches the library, the
network or files returns a promise. Each plugin has its own runtime; plugins share nothing.

Calls into a plugin (event handlers, the background task) are not run one after another: they
interleave at each `await`, as in any async JavaScript. A read-modify-write of storage that awaits
in between can lose another call's write. Use `zk.storage.update`, which runs a plugin's storage
writes strictly in order:

```ts
await zk.storage.update<number>("finished", (n = 0) => n + 1);
```

All of a plugin's storage writes (`set`, `delete`, `update`) go through one queue and apply in the
order they were asked. Inside an update's function, write only by returning the new value: calling
`set`, `delete` or `update` there rejects at once (it would wait for itself). `get` and `keys` are
fine.

## Permissions

A plugin declares what it may do in `plugin.json`. The user sees the list, in plain words, before
turning it on. Calling something not declared throws (or rejects) with a `zk.PermissionError` whose
message names the permission to add:

```
zk.on("highlightCreated") needs "annotations:read" in plugin.json
```

The plugin is never prompted for a permission while it runs. Only removals ask the user, each time.

| Permission | Lets the plugin | Shown as |
|---|---|---|
| `library:read` | read items, collections, tags, attachments; search; hear `book…` and `itemMarked…` events | Reads your library |
| `annotations:read` | read highlights and notes; hear `highlight…` events | Reads your highlights and notes |
| `fulltext:read` | read the text of downloaded books and papers | Reads the text of your books and papers |
| `notes:read` | read Zotero notes (HTML) | Reads your notes |
| `net:<host>` | `zk.fetch` to that host; `net:*.example.org` covers its subdomains (not `example.org` itself) | Connects to example.org |
| `net:*` | `zk.fetch` to any host on the internet | Connects to any website (it can send what it reads anywhere) |
| `zotero:create-items` | create items | Adds items to your library |
| `zotero:collections` | add items to collections | Files items into collections |
| `zotero:tags` | add tags | Adds tags |
| `zotero:notes` | add child notes | Adds notes to items |
| `zotero:remove` | *ask* to trash items, take them out of collections, remove tags, delete its own notes | May ask to move items to the trash or remove tags and notes — each time with your yes |
| `background` | run a task after syncs, at most hourly | Works in the background (hourly) |

Event permissions are checked when the handler is registered, so a missing one stops `main.js`
from loading, with the message above. Storage, files, logging, `zk.setBadge`, `syncFinished`,
`zk.zotero.cancel` and `zk.zotero.pending` need no permission.

### The network

`zk.fetch` is the web's `fetch`, with rules:

- HTTPS only, to declared hosts only. Every redirect (at most 5) is checked the same way; an
  `Authorization` header is dropped when a redirect leaves the origin.
- `zotero.org`, every `*.zotero.org` and the user's WebDAV host are always refused, even with
  `net:*`.
- With `net:*`, hosts on the local network (loopback, private and link-local addresses, checked on
  the address actually connected to) are refused. Declare such a host by name, e.g.
  `net:nas.local`, to reach it.
- No cookies, and none of the app's credentials. A user name or password in the URL is refused.
- A response may be at most 10 MB, and a request may take at most 30 s (less with
  `{ timeout }`). A plugin may have 8 fetches under way at once, with 20 MB of request bodies
  between them. Going over rejects that fetch with a `zk.LimitError`.
- HTTP errors don't reject: check `res.ok` or `res.status`. A network failure rejects with an `Error`.

## Reading the library

`zk.library` reads the library as it is on the Kindle: synced from Zotero, plus changes not sent
yet. Everything is a plain object, the same shape wherever it comes from (a query, an event).
Long lists are lazy async iterables:

```ts
for await (const item of zk.library.items({ tag: "to-read" })) console.log(item.title);
```

Annotations belong to an attachment (the PDF or EPUB), not to the item: list
`zk.library.attachments(item.key)`, then each one's `zk.library.annotations(attachment.key)`.
`zk.library.fullText` reads a downloaded book's text with the app's own engines, off the screen.

## Writing to Zotero

Plugins never write to Zotero. They ask the app; the app records the change, shows it in the
library at once, and the sync sends it to Zotero later, like a highlight made on the Kindle. Each
change is sent against the version the app knows. If the object changed in Zotero meanwhile, the
app fetches it again and makes the change again on Zotero's copy; it never overwrites an edit made
elsewhere. A change Zotero refuses (say, the item was deleted there) is marked *Refused* and never
retried.

Every write changes one thing and resolves with a `Change`, `{ key, changeId }`: the key of what
changed, and an id to withdraw it while it is unsent.

- **Additions** — `createItem`, `addToCollection`, `addTag`, `addNote` — are allowed by their
  permission and resolve at once.
- **Removals** — `trash`, `removeFromCollection`, `removeTag`, `deleteNote` — need the user's yes
  every time. The app shows its own confirmation over whatever is on screen, naming what goes:
  *"Feed Riffle wants to move 'Gaussian random fields (v1)' to the Zotero trash."* The plugin can't
  draw, skip or dismiss it. The call resolves with the `Change` if the user said yes, `null` if not:

  ```ts
  if (await zk.zotero.trash(old.key)) console.log("moved to the trash");
  ```

- **Editing** existing data (a title, a note, an annotation) isn't possible in API v0.

More about removals:

- They are only asked in answer to something the user just did on the Kindle: an event with
  `origin: "kindle"` (later, a plugin's own action or screen). Asked from the background task, from
  an event that came in with a sync, from a timer or while loading, they resolve `null` without
  asking (the log says so).
- Removals asked together (before awaiting any of them, e.g. with `Promise.all`) are confirmed
  together, in one list, with one answer for all.
- A removal that would change nothing (a tag the item doesn't have) resolves `null` without asking.
- Nothing is deleted for good except a note this plugin created (`Note.createdBy`). Items go to
  Zotero's trash, where the user can restore them.
- Time spent waiting for the answer doesn't count against the call's time budget.

**Withdrawing.** `zk.zotero.cancel(changeId)` withdraws one of the plugin's own changes that hasn't
been sent, without asking: nothing leaves Zotero. Withdrawing an addition takes it back (a new
item goes with what was added to it); withdrawing a removal the user agreed to puts back what it
removed. It resolves `false` if the change was already sent. Undoing a sent addition is a removal:
ask with `trash` and friends. `zk.zotero.pending()` lists the plugin's unsent changes.

**What it changed.** Every change a plugin asks for is listed on its page in Settings → Plugins
→ *What it changed*, with what became of it: Pending, Sent, Refused (with Zotero's reason),
Cancelled (withdrawn by the plugin) or Declined (the user said no).

Turning a plugin off or removing it doesn't drop its unsent changes: they were asked while it was
on, and the sync sends them without it. The user can drop them with *Discard unsent changes*.

## Events

`zk.on(event, handler)` runs the handler whenever the event happens. Register handlers at the top
level of `main.js`.

| Event | When | Permission |
|---|---|---|
| `highlightCreated` | annotations were made | `annotations:read` |
| `highlightChanged` | annotations were changed (comment, colour, text) | `annotations:read` |
| `highlightDeleted` | annotations were deleted (only their keys are left) | `annotations:read` |
| `bookOpened` | a book or paper was opened on the Kindle | `library:read` |
| `bookFinished` | the user turned past its last page | `library:read` |
| `itemMarkedRead` | items were marked read | `library:read` |
| `itemMarkedUnread` | items were marked unread | `library:read` |
| `syncFinished` | a sync with Zotero finished, after that sync's other events | none |

Every event says where the change was made:

- `origin: "kindle"` — the user did it on this Kindle. One event per change, as it happens, with
  one entry in its list (`annotations` or `items`).
- `origin: "zotero"` — it came in with a sync. One event per kind per sync, listing everything (at
  most 500 entries an event; more come in further events).

The first full sync of a library fires only `syncFinished` with `initial: true`, never thousands of
events; neither do the app's catch-up downloads (turning on *Keep all highlights offline*, a
book's first download of its annotations). Catch up with library reads, for example in the
background task. A sync that stops with an error sends no `syncFinished`. `bookOpened` and
`bookFinished` happen only on the Kindle; `bookFinished` fires once each time the book is open.

A handler has 5 seconds. For a sync's long lists, note what to do and do it in the background
task; [Highlights to Markdown](../../sdk/examples/highlights-to-markdown/src/main.ts) shows how.

## Background work

```ts
zk.background(async () => {
  const fresh = await refreshFeeds();
  zk.setBadge(fresh ? `${fresh} new` : null);
});
```

A plugin with the `background` permission registers one task. The app runs it after a successful
sync, at most once an hour (syncs happen every 10 minutes by default), while the Kindle is awake
and online. It never wakes a sleeping Kindle. A run has 30 seconds.

- No run while the app knows it is offline. A run that fails because the connection dropped (the
  error `zk.fetch` rejected with, not caught) isn't counted as a failure and runs again after the
  next sync — up to 3 times in a row; the 4th counts.
- A run cut short by the Kindle going to sleep or the app closing isn't counted either, and is
  tried again next time.
- The background task can't show anything or ask the user: removals resolve `null`. Use
  `zk.setBadge("3 new")` to say there is something to see. A badge is cut to 40 characters, kept
  across restarts, and shown with the plugin in Settings → Plugins; `null` clears it.

## Storage and files

- `zk.storage`: a private key-value store of JSON values. At most 1 MB a value and 16 MB in all.
- `zk.files`: text files in the plugin's own `data/` folder, for exports the user copies off over
  USB. Paths are relative to `data/`; a path that leaves it is refused. At most 16 MB a file.

Both survive restarts and app updates. *Reset its data* in Settings → Plugins empties both.

## Limits, and what happens when they're hit

| Limit | Value |
|---|---|
| Time, event handler (and loading `main.js`) | 5 s |
| Time, background task | 30 s |
| Time, actions (part 2) | at least 30 s |
| Memory | 32 MB a plugin; 96 MB for all plugins together |
| Stack | deep recursion is stopped |
| `zk.fetch` | 10 MB a response, 30 s a request, 8 at once, 20 MB of request bodies |
| `zk.storage` | 1 MB a value, 16 MB in all |
| `zk.files` | 16 MB a file |

A call's time counts from when it starts running. Time queued behind other plugins' calls doesn't
count, nor does time waiting for the user to answer a confirmation. Awaited work (fetches, library
reads) does count. A timer that fires while the call that set it is still running counts against
that call; one that fires later runs as a call of its own, with 5 seconds.

**A call that goes over** (time, memory or stack) is stopped:

- The work it started (fetches, library reads, storage and file operations, timers) is cancelled,
  and every promise of that work still pending rejects with a `zk.LimitError` (its `limit` says
  which). None is left pending forever, so queues move on.
- Changes to Zotero it already asked for stay asked for; a removal still waiting for the user's
  answer is withdrawn.
- If the stop cut into running code, the plugin is restarted: `main.js` runs afresh, module
  variables start over, `zk.storage` keeps what it stored.

**Failures.** An uncaught error in a handler, the background task or `main.js`, and every limit
stop, is logged with its stack and counted. Three in a row turn the plugin off, with the note
*Turned off — it stopped working* in Settings → Plugins; a success resets the count. A plugin that
fails while loading isn't started again until it is reloaded or turned on again. A rejected
promise nobody awaited is logged as a warning (`Uncaught (in promise) …`) and not counted.

**Crashes.** If the app ever stops while a plugin's code is running, the next start turns that
plugin off (*The app stopped while … was running — turned off*). If the app fails to start, or
crashes, twice in a row while plugins are on, the next start runs with all plugins off. Nothing a
plugin does needs a reinstall to undo; see [Using plugins](using.md#if-something-goes-wrong).

## Entry points for later

`zk.selectionAction`, `zk.itemAction` and `zk.screen` are part of API v0 but nothing shows them
yet: plugin screens, the selection bar and an item's *More…* come in the next part. Calling them
now checks their arguments and records them, so a plugin written today keeps working.
