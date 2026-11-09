# Recipes

Short, complete plugins for common jobs. Each is the whole `src/main.ts` of a plugin made from the
template (see [Getting started](getting-started.md)); its first lines say which permissions to put
in `plugin.json`. The sources are in [`sdk/recipes/`](../../sdk/recipes/), where `sdk/check.sh`
type-checks them against the API, so they can't go stale.

- [Export highlights](#export-highlights)
- [Call a web API with a key from settings.json](#call-a-web-api-with-a-key-from-settingsjson)
- [File an item into a collection, with undo](#file-an-item-into-a-collection-with-undo)
- [React to finishing a book](#react-to-finishing-a-book)
- [Run something hourly](#run-something-hourly)

## Export highlights

Each time you highlight a book on the Kindle, its highlights and comments are written to
`<citation key or title>.md` in the plugin's `data/` folder. Copy them off over USB from
`zotero/plugins/<id>/data/`.

Highlights that arrive with a sync come in one event that may list hundreds, too many for a
handler's 5 seconds, so this recipe leaves them out. The example plugin
[Highlights to Markdown](../../sdk/examples/highlights-to-markdown/src/main.ts) handles them too,
with a background task, and keeps file names unique.

<!-- recipe: sdk/recipes/export-highlights.ts -->
```ts
// Export a book's highlights to Markdown each time you highlight it on the Kindle.
// plugin.json: "permissions": ["annotations:read", "library:read"]

zk.on("highlightCreated", async ({ origin, annotations }) => {
  // A sync can bring hundreds of highlights at once (origin "zotero"); a handler has 5 seconds.
  // This recipe handles the Kindle's own, one at a time.
  if (origin !== "kindle") return;
  for (const itemKey of new Set(annotations.map((a) => a.itemKey))) await exportBook(itemKey);
});

async function exportBook(itemKey: string): Promise<void> {
  const item = await zk.library.item(itemKey);
  if (!item) return;
  const lines = [`# ${item.title}`, ""];
  // Highlights belong to the item's attachments (its PDF or EPUB), in reading order.
  for (const attachment of await zk.library.attachments(item.key)) {
    for (const a of await zk.library.annotations(attachment.key)) {
      if (a.text) lines.push(`> ${a.text.trim().replace(/\n/g, "\n> ")}`, "");
      if (a.comment) lines.push(a.comment, "");
    }
  }
  const name = (item.citationKey ?? item.title).replace(/[\\/:*?"<>|]+/g, " ").trim() || item.key;
  await zk.files.write(`${name}.md`, lines.join("\n"));
  console.log(`wrote ${zk.plugin.dataDir}/${name}.md`);
}
```

## Call a web API with a key from settings.json

Each highlight you make is translated with [DeepL](https://www.deepl.com/pro-api), with your own
API key, and the translations are kept in `data/translations.md`. The key is the user's: it goes in
`settings.json`, next to `plugin.json` on the Kindle, never in the code:

```json
{ "deeplKey": "your key", "targetLang": "EN-GB" }
```

The plugin may only connect to the one host it declares. Merging the settings' fields into
`zk.Settings` types `zk.settings`. If the key is missing, the handler throws an error that tells
the user what to do; it shows on the plugin's page in Settings → Plugins. (Three failures in a
row turn the plugin off, so a plugin used for long without its settings would rather log a
warning and return.)

<!-- recipe: sdk/recipes/web-api-key.ts -->
```ts
// Translate each highlight you make on the Kindle with DeepL, using your own API key, and keep
// the translations in a Markdown file.
// plugin.json: "permissions": ["annotations:read", "net:api-free.deepl.com"]
// settings.json (next to plugin.json): { "deeplKey": "your key", "targetLang": "EN-GB" }

// Describe settings.json, so zk.settings is typed.
declare global {
  namespace zk {
    interface Settings {
      deeplKey?: string;
      targetLang?: string;
    }
  }
}

zk.on("highlightCreated", async ({ origin, annotations }) => {
  if (origin !== "kindle") return;
  const key = zk.settings.deeplKey;
  if (!key) throw new Error('Put your DeepL key in settings.json: { "deeplKey": "…" }');
  const texts = annotations.flatMap((a) => (a.text ? [a.text] : []));
  if (texts.length === 0) return;

  const res = await zk.fetch("https://api-free.deepl.com/v2/translate", {
    method: "POST",
    headers: { "content-type": "application/json", authorization: `DeepL-Auth-Key ${key}` },
    body: JSON.stringify({ text: texts, target_lang: zk.settings.targetLang ?? "EN-GB" }),
    timeout: 4000, // the handler has 5 seconds in all
  });
  if (!res.ok) throw new Error(`DeepL: HTTP ${res.status}`);
  const { translations } = await res.json<{ translations: { text: string }[] }>();

  // Keep them in storage (update: safe if two highlights come in at once), then rewrite the file.
  const all = await zk.storage.update<string[]>("translations", (old = []) => [
    ...old,
    ...translations.map((t, i) => `> ${texts[i]}\n\n${t.text}\n`),
  ]);
  await zk.files.write("translations.md", (all ?? []).join("\n"));
});
```

## File an item into a collection, with undo

Marking a book read on the Kindle files it into your *Read* collection; marking it unread undoes
that. Filing is an addition: allowed by its permission, done at once. Undoing first tries
`zk.zotero.cancel`, which withdraws the change without asking if it hasn't synced yet. If it has,
taking the book out of the collection is a removal, so the app asks the user; the call resolves
`null` if they say no. That works here because marking unread is something the user just did on
the Kindle (`origin: "kindle"`).

<!-- recipe: sdk/recipes/file-with-undo.ts -->
```ts
// Mark a book read on the Kindle and it is filed into your "Read" collection; mark it unread
// again and that is undone: withdrawn if it hasn't synced yet, otherwise you are asked.
// plugin.json: "permissions": ["library:read", "zotero:collections", "zotero:remove"]

async function readCollection(): Promise<zk.Collection | undefined> {
  return (await zk.library.collections()).find((c) => c.path.join("/") === "Read");
}

zk.on("itemMarkedRead", async ({ origin, items }) => {
  if (origin !== "kindle") return;
  const read = await readCollection();
  if (!read) throw new Error('Make a collection called "Read" in Zotero first');
  for (const item of items) {
    if (item.collections.includes(read.key)) continue;
    // An addition: allowed by the permission, resolves at once, syncs later.
    const change = await zk.zotero.addToCollection(item.key, read.key);
    // Keep its changeId: the way to withdraw it while it is unsent.
    await zk.storage.set(`filed:${item.key}`, change.changeId);
  }
});

zk.on("itemMarkedUnread", async ({ origin, items }) => {
  if (origin !== "kindle") return;
  const read = await readCollection();
  for (const item of items) {
    const changeId = await zk.storage.get<string>(`filed:${item.key}`);
    if (!changeId || !read) continue;
    await zk.storage.delete(`filed:${item.key}`);
    if (await zk.zotero.cancel(changeId)) {
      console.log(`"${item.title}" won't be filed after all`);
    } else if (await zk.zotero.removeFromCollection(item.key, read.key)) {
      // Already synced: taking it out is a removal, so the app asked and the user said yes.
      console.log(`took "${item.title}" out of Read`);
    }
  }
});
```

## React to finishing a book

`bookFinished` fires when you turn past the last page. This recipe tags the book and adds a note
with the date. Both are additions, sent to Zotero with the next sync; the plugin's page in
Settings → Plugins lists them under *What it changed*.

<!-- recipe: sdk/recipes/finished-book.ts -->
```ts
// When you turn past the last page of a book, tag it and add a note with the date.
// plugin.json: "permissions": ["library:read", "zotero:tags", "zotero:notes"]

zk.on("bookFinished", async ({ item }) => {
  await zk.zotero.addTag(item.key, "finished-on-kindle");
  const day = new Date().toISOString().slice(0, 10);
  await zk.zotero.addNote(item.key, `<p>Finished on the Kindle on ${day}.</p>`);
  console.log(`finished "${item.title}"`);
});
```

## Run something hourly

The background task runs after a successful sync, at most once an hour, while the Kindle is awake
and online, and has 30 seconds. This one adds the highlights made since its last run to a digest
file per day, `data/digest/<date>.md`, and sets the plugin's badge. It remembers its last run in
`zk.storage`; the first run only starts the clock.

<!-- recipe: sdk/recipes/hourly.ts -->
```ts
// Once an hour (after a sync), add the highlights made since the last run to a digest file per
// day, and show how many were added on the plugin's badge.
// plugin.json: "permissions": ["annotations:read", "library:read", "background"]

zk.background(async () => {
  const startedAt = new Date().toISOString();
  const since = await zk.storage.get<string>("lastRun");
  if (!since) {
    // The first run: start from now rather than from the whole library.
    await zk.storage.set("lastRun", startedAt);
    return;
  }

  const lines: string[] = [];
  // Newest change first, read lazily, stopping at `since`.
  for await (const a of zk.library.recentAnnotations({ since, limit: 200 })) {
    if (!a.text) continue;
    const item = await zk.library.item(a.itemKey);
    lines.push(`- "${a.text.trim()}" (${item?.title ?? "?"})`);
  }
  if (lines.length > 0) {
    const name = `${startedAt.slice(0, 10)}.md`;
    const old = (await zk.files.list("digest")).includes(name) ? await zk.files.read(`digest/${name}`) : "";
    await zk.files.write(`digest/${name}`, old + lines.join("\n") + "\n");
  }
  await zk.storage.set("lastRun", startedAt);
  zk.setBadge(lines.length > 0 ? `${lines.length} new` : null);
});
```
