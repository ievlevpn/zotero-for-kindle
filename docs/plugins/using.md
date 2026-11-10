# Using plugins

Plugins add things to Zotero for Kindle that aren't in the app itself: exports, feeds, small
automations. A plugin is a folder you copy onto the Kindle. It runs inside the app and can only
do what it declares; anything that removes something from your library needs your yes each time.

Plugins are code written by other people. Install the ones you trust, and read what each may do
before you turn it on.

There are two ways to get one: from the catalog inside the app, or by copying a folder over USB.

## Get plugins (the catalog)

**Settings → Plugins → Get plugins** (the last row) lists the plugins in the catalog, by name, with
the author and a line about each. A plugin you have shows **Installed**; tapping it opens its own
page, where its updates are. At the top: *As of <date>* and **Check now**. With no Wi-Fi and no copy of the list
yet it says *Connect to Wi-Fi to see the plugins in the catalog.*; after that the list shows offline.

1. Tap a plugin, read about it, and tap **Install**. It reads *Downloading…* while it works.
2. The plugin's page opens. It is **off**: read what it can do and tap **Turn on**, exactly as for
   a plugin you copied by hand.

Every download is checked against its author's key, which the catalog pins. If that fails, nothing
is installed, and the page says why in a sentence, such as *The download isn't signed by the
plugin's author.*, *It needs app 0.5.0.* or *No Wi-Fi.*

### Updates

Settings → Plugins marks a plugin that has a newer version with *Update to 1.3*, and shows
**Update all (n)** at the top. Opening a plugin's page checks the catalog too, when the app hasn't
in the last hour, so a newer version shows there without a trip to Get plugins. Updates are always
a tap, never automatic.

- Open the plugin and tap **Update to 1.3**. If the new version asks for nothing more than the old
  one did, it installs at once and an enabled plugin stays enabled.
- If it asks for more, you are told what: *This version also wants to: …*, with **Update** and
  **Not now**. The old version keeps running until you say yes. **Update all** skips these; open
  them one at a time.

Its settings and data stay with it across an update.

### Roll back

An update keeps the version before it. On the plugin's page, **Roll back to 1.2** puts that
version back, with the plugin's data as it is now. One earlier version is kept.

### Remove

On the plugin's page, **Remove** asks *Remove <name> and the data it saved?*; on yes the plugin's
folder, data and storage are deleted. Changes it asked for that haven't synced are still sent.

### From the catalog, or installed by hand

A plugin's page says which it is: **From the catalog** (installed with Get plugins, so it can
update and roll back) or **Installed by hand** (copied over USB). If a hand-installed plugin's id is
in the catalog, it is offered the catalog's updates too, signed by the catalog's author; an update
that asks for more permissions asks you first.

### Withdrawn plugins

If a plugin is found to be harmful, the catalog can withdraw it. At the next check it is turned
off with *Turned off — withdrawn from the catalog: <reason>*. It isn't deleted; you remove it
yourself if you want to, and it can't be turned on again.

## Installing a plugin by hand

1. Close Zotero on the Kindle (the × at the top right of Home).
2. Connect the Kindle by USB (as in [INSTALL.md](../../INSTALL.md#2-copy-them-onto-the-kindle):
   a drive, or OpenMTP on a Mac for newer Kindles).
3. Copy the plugin's folder into `zotero/plugins/` on the Kindle, so that you have
   `zotero/plugins/<its id>/plugin.json` and `main.js` next to it. Make the `plugins` folder if it
   isn't there. The folder's name must be the plugin's id (it says which in its instructions, and
   in `plugin.json`).
4. Eject the Kindle and start Zotero.
5. Open **Settings → Plugins**. The new plugin is listed with *New — not turned on yet*. Tap it,
   read what it can do, and tap **Turn on**.

A plugin starts when it is first needed, for example at the next sync.

**Updating a plugin installed by hand:** copy the new version's files over the old ones, the same way, with the app
closed. It stays on, and keeps its settings and data. A new version may ask for more than the old
one did: look at its page in Settings → Plugins again.

## Its settings

Some plugins need settings, such as an API key for a web service. They go in a file
`settings.json` in the plugin's folder, next to `plugin.json`; the plugin's instructions say what
to put in it. For example:

```json
{ "deeplKey": "your key", "targetLang": "EN-GB" }
```

Edit it over USB. The plugin reads it when it starts: after a change, turn the plugin off and on
again, or restart Zotero.

## What a plugin may do

A plugin's page lists what it may do, in these words:

| You see | It may |
|---|---|
| Reads your library | read your items, collections and tags, and search them |
| Reads your highlights and notes | read your annotations |
| Reads the text of your books and papers | read the text of books downloaded to the Kindle |
| Reads your notes | read your Zotero notes |
| Connects to example.org | send and receive data from that site only (*sites under example.org*: its subdomains) |
| Connects to any website (it can send what it reads anywhere) | use any site on the internet. Together with reading your library, that means it could send your library anywhere. |
| Adds items to your library | create new items |
| Files items into collections | add items to collections |
| Adds tags | add tags to items |
| Adds notes to items | add notes to items |
| May ask to move items to the trash or remove tags and notes — each time with your yes | ask you, each time, before it trashes an item, takes one out of a collection, removes a tag or deletes a note it added |
| Works in the background (hourly) | run after a sync, at most once an hour, while the Kindle is awake and online |

What no plugin can do, whatever it declares: see your Zotero API key or WebDAV password, talk to
zotero.org or your WebDAV server, read files outside its own folder, change existing items, notes
or highlights, delete an item for good, draw on the screen, or wake the Kindle.

Its changes to your library go through the app, which sends them with the sync like a highlight
made on the Kindle, and never over an edit you made in Zotero.

## When a plugin asks to remove something

If a plugin wants to move an item to the Zotero trash, take it out of a collection, remove a tag
or delete a note it added, the app asks you first, in a box of its own over the screen, naming
what goes:

> Feed Riffle wants to move 'Gaussian random fields (v1)' to the Zotero trash.
>
> **Cancel** · **Move to trash**

Cancel is on the left, the change on the right. The plugin can't skip, dismiss or answer the box.
It only answers to your finger (not the pen), and not in the first half second, so a tap meant
for the page doesn't answer it. If you are turning a page, it waits for the turn to finish (up to
3 seconds). It only comes right after something you did. Items go to Zotero's trash, from where you can restore them in Zotero.

## Where plugins show up

- **Reader:** select text, then **More…** in the selection bar lists plugin actions.
- **Item details:** the **Plugins** section lists actions for that item.
- **Home:** the puzzle-piece icon opens the plugins' own screens (a dot on it means a plugin has
  something new).

An action shows **Working…** while it runs; tap it to stop. The answer comes in a pop-up, with up to
two buttons of the plugin's. If the plugin fails you see "<Plugin> couldn't do that: <message>".

## Turning plugins on and off

- In **Settings → Plugins**, the switch on each row turns a plugin on or off. (The first time, the
  row opens its page, so you see what it may do.)
- On its page: **Turn on** / **Turn off**.
- At the top of the list: **Turn all plugins off**.

A plugin that's off doesn't run at all. Changes it asked for while it was on, not yet synced, are
still sent (see *Discard unsent changes* below).

## A plugin's page

- Its description, version and author, and what it may do.
- Its badge, a short note it may set, such as *3 new*.
- *Last errors*: its last three errors, if it has had any.
- **What it changed ›**: every change it asked for in your library, newest first, and what became
  of it: *Pending* (not synced yet), *Sent*, *Refused* (Zotero said no, with why), *Cancelled* (the
  plugin withdrew it before it was sent) or *Declined* (you said no).
- **Reset its data**: deletes what it stored on the Kindle. Its changes in Zotero stay.
- **Discard N unsent changes**: drops the changes it asked for that haven't synced. They won't
  reach Zotero.
- **Remove**: deletes its folder. Changes not synced yet are still sent; the question says how many.

Each of the last three asks first.

## If something goes wrong

Nothing a plugin does needs a reinstall of the app to undo.

- **A plugin keeps failing.** After three failures in a row the app turns it off. Settings →
  Plugins says *Turned off — it stopped working*, and its page shows the errors. Turn it on again
  once you've updated it, or remove it.
- **The app stopped while a plugin was running.** The next start turns that plugin off: *The app
  stopped while … was running — turned off*.
- **The app keeps stopping.** If it fails to start, or crashes, twice in a row while plugins are
  on, the next start turns all plugins off, and Home says *Plugins were turned off because the app
  stopped. Settings → Plugins*. Turn them back on one at a time, starting with the ones you trust.
- **Keep all plugins off.** Over USB, create a file named `OFF` (any content, even empty) in
  `zotero/plugins/`. The app starts with every plugin off until you delete the file.
- **A plugin's data is in a bad state.** On its page: **Reset its data**.
- **Its queued changes are wrong.** On its page: **Discard unsent changes**.
- **Remove it.** On its page: **Remove**. Or, over USB, delete its folder in `zotero/plugins/`;
  at the next start the app also drops its kept previous version and what it remembered of it (a
  copy put back is new, and off). That removes every trace of it except the changes already in
  Zotero.
- **After an app update** a plugin made for an older plugin API is listed as *Incompatible* and
  isn't run, instead of failing. App updates and rollbacks never touch `zotero/plugins/`.

To report a problem with a plugin, its log is `zotero/plugins/<id>/plugin.log` on the USB drive.

## Where things are

```
zotero/plugins/              on the Kindle's USB drive (/mnt/us/zotero/plugins)
  OFF                        if present: all plugins off
  <id>/
    plugin.json  main.js     the plugin
    settings.json            your settings for it (if it has any)
    data/                    files it wrote, e.g. exports to copy off
    storage.db  plugin.log   what it keeps, and its log
```
