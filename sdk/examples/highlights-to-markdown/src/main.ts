// Highlights to Markdown — the example plugin that ships with Zotero for Kindle.
//
// It keeps one Markdown file per book in the plugin's data folder, with the book's highlights
// and comments in reading order:
//
//   /mnt/us/zotero/plugins/highlights-to-markdown/data/adler2007.md
//
// Copy the folder off over USB and the files drop into Obsidian or any notes app. A book's file
// is named after its citation key (Zotero's, or Better BibTeX's) or, without one, after its
// title.
//
// Read this file top to bottom: it is a walk-through of the plugin API. Every `zk.…` call is
// documented, with an example, in sdk/types/zk.d.ts.
//
// The plan, in four pieces (the sections below):
//
//   1. State         what the plugin remembers in zk.storage, chiefly a "to do" list of books.
//   2. Writing       exportBook() writes one book's file from what the library holds now. It
//                    is the only thing that writes, and doing it twice does no harm.
//   3. Events        a highlight made on the Kindle is noted and written at once; highlights
//                    that arrive with a sync (made in Zotero on the desktop) are only noted.
//                    When the sync finishes, a few noted books are written straight away.
//   4. Background    after a sync, at most hourly, writes what is noted, and catches anything
//                    missed (changes made while the plugin was off; on its first run, every
//                    book in the library).
//
// Permissions (plugin.json): "annotations:read" to hear highlight events and read annotations,
// "library:read" to read each book's title, authors and attachments, "background" for piece 4.
// Writing files to its own data folder and using its own storage need no permission.

// ───────────────────────────── 1. What the plugin remembers ─────────────────────────────

// Plugins keep their state in zk.storage, a key-value store of JSON values that survives
// restarts. This plugin keeps one value under one key, "state":
interface State {
  /** Every book written so far: item key → its file. Lets us rename or remove a file later. */
  books: Record<string, { file: string; highlights: number; written: string }>;
  /** Item keys of books to write at the next chance. */
  todo: string[];
  /** When the background task last looked for changes (ISO 8601); absent before its first run. */
  lastRun?: string;
}

const empty = (): State => ({ books: {}, todo: [] });

async function loadState(): Promise<State> {
  // get<T>() returns what was stored, or undefined the first time.
  return (await zk.storage.get<State>("state")) ?? empty();
}

// Event handlers and the background task can run at the same time: each `await` lets another
// one in. Two of them loading the state, changing it and saving it with get and set would lose
// one change. So every change goes through zk.storage.update, which hands `change` the stored
// state and stores what it returns, one update at a time. Keep each update short — one book —
// because an event handler waiting its turn behind a long one would run out of its 5 seconds.
async function updateState(change: (state: State) => void | Promise<void>): Promise<void> {
  await zk.storage.update<State>("state", async (old) => {
    const state = old ?? empty(); // a fresh copy each time, parsed from storage
    await change(state);
    return state;
  });
}

// ───────────────────────────── 2. Writing one book ─────────────────────────────

/**
 * Writes (or rewrites) the file of the book `itemKey` and records it in `state.books`. A book
 * with no highlights left, or no longer in the library, loses its file.
 */
async function exportBook(itemKey: string, state: State): Promise<void> {
  const previous = state.books[itemKey];

  // An item, as everywhere in the API, is a plain object: title, creators, year, extra…
  // item() resolves null if the key isn't in the library (or the item is in the trash).
  const item = await zk.library.item(itemKey);

  // Highlights belong to an attachment (the PDF or EPUB), not to the item: list the item's
  // attachments, then each one's annotations, in reading order.
  const parts: { attachment: zk.Attachment; annotations: zk.Annotation[] }[] = [];
  if (item) {
    for (const attachment of await zk.library.attachments(item.key)) {
      // Keep what has words in it: highlights, underlines, notes. Image and ink annotations
      // have neither text nor (usually) a comment.
      const annotations = (await zk.library.annotations(attachment.key)).filter((a) => a.text || a.comment);
      if (annotations.length > 0) parts.push({ attachment, annotations });
    }
  }
  const highlights = parts.reduce((n, p) => n + p.annotations.length, 0);

  if (!item || highlights === 0) {
    if (previous) {
      // zk.files works inside the plugin's own data/ folder only; paths are relative to it.
      await zk.files.remove(previous.file);
      delete state.books[itemKey];
      console.log("removed", previous.file);
    }
    return;
  }

  const file = fileNameFor(item, state);
  await zk.files.write(file, toMarkdown(item, parts));
  if (previous && previous.file !== file) await zk.files.remove(previous.file); // renamed
  state.books[itemKey] = { file, highlights, written: new Date().toISOString() };
  console.log(`wrote ${file} (${highlights} highlights)`);
}

/**
 * Writes the given books one by one, each as its own job: write the file, take the book off the
 * "to do" list, save. A call stopped halfway leaves the rest noted for next time.
 */
async function exportBooks(itemKeys: Iterable<string>): Promise<void> {
  for (const key of new Set(itemKeys)) {
    await updateState(async (state) => {
      await exportBook(key, state);
      state.todo = state.todo.filter((k) => k !== key);
    });
  }
}

/** Notes books to write. Cheap, so it can always run first. */
async function addTodo(itemKeys: Iterable<string>): Promise<void> {
  await updateState((state) => {
    state.todo = [...new Set([...state.todo, ...itemKeys])];
  });
}

// ───────────────────────────── 3. Events ─────────────────────────────

// Three events carry annotations: made, changed, deleted. Each says where the change happened:
//   origin "kindle" — the user just did it on this Kindle; one event per highlight, at once.
//   origin "zotero" — it came in with a sync; one event for everything in that sync, which
//                     can be hundreds of highlights. A handler has 5 seconds, so for those we
//                     only note the books; the background task writes them (piece 4).
// Kindle changes are noted first too, then written: if the handler runs out of time (say,
// waiting behind the background task), the book stays noted and isn't lost.
// A deleted annotation is gone from the library, so highlightDeleted carries only its keys —
// which is all we need: the item key says which book to rewrite.
async function onAnnotations(origin: zk.Origin, annotations: { itemKey: string }[]): Promise<void> {
  const books = annotations.map((a) => a.itemKey);
  await addTodo(books);
  if (origin === "kindle") await exportBooks(books);
}

zk.on("highlightCreated", ({ origin, annotations }) => onAnnotations(origin, annotations));
zk.on("highlightChanged", ({ origin, annotations }) => onAnnotations(origin, annotations));
zk.on("highlightDeleted", ({ origin, annotations }) => onAnnotations(origin, annotations));

// syncFinished fires after the other events of a sync, so the books they noted are waiting.
// The background task writes them, but runs at most once an hour; a sync every 10 minutes
// usually brings a book or two, so write a few now — few enough for 5 seconds. The rest wait
// for the background task, which has 30 seconds a run.
// The first full sync of a library fires nothing but this event (with initial: true) — no
// highlight events for the thousands of annotations it brings. That case is left to the
// background task, whose first run writes every book.
const WRITE_AFTER_SYNC = 3;
zk.on("syncFinished", async ({ initial }) => {
  if (initial) return;
  await exportBooks((await loadState()).todo.slice(0, WRITE_AFTER_SYNC));
});

// ──────────────────── 4. Background: write what is noted, catch what was missed ────────────────────

// The background task runs after a sync, at most once an hour. It finds every book with an
// annotation changed since its last run (all books, the first time) and notes them as "to do" —
// moving `lastRun` on in the same step, since the list now carries that work. Then it works
// through "to do". If the 30 seconds run out, what is left stays noted, and the next run carries
// on from there instead of starting over.
//
// recentAnnotations() is lazy (`for await`): it reads the library a page at a time, newest
// change first, and stops at `since`. Deleted annotations aren't in it — the events above
// take care of those.
zk.background(async () => {
  const started = new Date().toISOString();
  const { lastRun } = await loadState();
  const changed = new Set<string>();
  for await (const a of zk.library.recentAnnotations({ since: lastRun })) changed.add(a.itemKey);
  await updateState((state) => {
    state.todo = [...new Set([...state.todo, ...changed])];
    state.lastRun = started;
  });

  await exportBooks((await loadState()).todo);
});

// ───────────────────────────── Markdown ─────────────────────────────

/**
 * A file name for the book: `<citation key>.md`, else `<title>.md`, made safe for FAT32 (the
 * Kindle's USB drive). If another book already has that name, the item key is added.
 */
function fileNameFor(item: zk.Item, state: State): string {
  const base = (item.citationKey ?? item.title)
    .replace(/[\\/:*?"<>|\u0000-\u001f]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 100) || item.key;
  const taken = Object.entries(state.books).some(([key, b]) => key !== item.key && b.file === `${base}.md`);
  return taken ? `${base} (${item.key}).md` : `${base}.md`;
}

function authors(item: zk.Item): string {
  const names = item.creators
    .filter((c) => c.type === "author" || c.type === "editor")
    .map((c) => c.name ?? [c.first, c.last].filter(Boolean).join(" "));
  return names.length > 3 ? `${names[0]} et al.` : names.join(", ");
}

/**
 * The file's text:
 *
 *   # Title
 *   Authors (2007). *Publication*. Citation key: `adler2007`
 *
 *   > highlighted text
 *
 *   The comment. — p. 12
 */
function toMarkdown(item: zk.Item, parts: { attachment: zk.Attachment; annotations: zk.Annotation[] }[]): string {
  const byline = [
    [authors(item), item.year ? `(${item.year})` : ""].filter(Boolean).join(" "),
    item.publication ? `*${item.publication}*` : "",
    item.citationKey ? `Citation key: \`${item.citationKey}\`` : "",
  ].filter(Boolean);
  const lines = [`# ${item.title || item.key}`, ""];
  if (byline.length) lines.push(`${byline.join(". ")}`, "");

  for (const { attachment, annotations } of parts) {
    if (parts.length > 1) lines.push(`## ${attachment.title}`, "");
    for (const a of annotations) {
      if (a.text) lines.push(...a.text.trim().split("\n").map((line) => `> ${line}`), "");
      const where = a.pageLabel ? `— p. ${a.pageLabel}` : "";
      const comment = a.comment?.trim() ?? "";
      if (comment || where) lines.push([comment, where].filter(Boolean).join(" "), "");
    }
  }
  return lines.join("\n");
}
