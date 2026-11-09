/**
 * Zotero for Kindle — plugin API v0.
 *
 * **v0 is unstable**: names and shapes may still change; every change is listed in the API
 * changelog. A plugin declares the version it was written for in `plugin.json` (`"api": 0`).
 *
 * A plugin is one ES module (`main.js`) run by the app on QuickJS-ng (ES2023). It is neither Node
 * nor a browser: there is no `require`, no `process`, no `window` or DOM. Rely on standard
 * ES2023 and on what this file declares: the one global `zk`, plus `console`, `setTimeout` /
 * `clearTimeout`, `URL`, `URLSearchParams`, `TextEncoder` and `TextDecoder` (only the members
 * declared below). Libraries bundled into `main.js` are fine if they need nothing else.
 *
 * Everything is plain data: items, notes and annotations are JSON-like objects, the same shape
 * wherever they come from (a query, an event, an action). Everything that touches the library,
 * the network or files is `async`, because plugins run on their own thread and the screen never
 * waits for them.
 *
 * The module is run once and stays loaded while the plugin is on, so module-level variables
 * last from one call to the next (but not across restarts: keep what must last in
 * {@link zk.storage}). Calls into the plugin (event handlers, the background task, actions) may
 * interleave at each `await`, as in any async JavaScript: a read-modify-write of storage that
 * awaits in between can lose another call's write, so do it with {@link zk.Storage.update},
 * which runs a plugin's updates strictly one after another.
 *
 * Each call has a time budget (event handlers 5 s, the background task 30 s, actions at least
 * 30 s). It counts from when the call starts running: time queued behind other plugins' calls
 * doesn't count, and neither does time waiting for the user to answer a confirmation. A call
 * that goes over is stopped (see {@link zk.LimitError}).
 *
 * What a plugin may do is declared in `plugin.json` under `"permissions"`. Calling something not
 * declared rejects (or throws) with a {@link zk.PermissionError} that names what to add. Each
 * member below says which permission it needs.
 *
 * @example
 * // main.ts — log every book finished on the Kindle (needs "library:read")
 * zk.on("bookFinished", ({ item }) => {
 *   console.log(`Finished ${item.title} (${item.year ?? "n.d."})`);
 * });
 */

/**
 * Writes to the plugin's log (Settings → Plugins → the plugin, and the terminal of the desktop
 * build). Arguments are joined with spaces; objects are shown as JSON. `warn` and `error` are
 * marked as such in the log. The one way to log: standard, with levels, and what bundled
 * libraries call. No permission.
 *
 * @example
 * console.log("filed", change.key, "into", collection.path.join(" / "));
 * console.warn("feed returned no entries", url);
 */
declare var console: {
  log(...data: unknown[]): void;
  warn(...data: unknown[]): void;
  error(...data: unknown[]): void;
};

// ───────────────────────────── Standard globals ─────────────────────────────
// Only the members declared here are provided.

/**
 * Runs `callback` after `ms` milliseconds, on the plugin thread. While the call that set it (an
 * event handler, the background task, an action) is still running, the callback counts against
 * that call's time budget; a timer firing after that call has finished runs as a call of its own,
 * with 5 seconds. Pending timers are dropped when the call that set them is stopped by a limit,
 * and when the plugin is turned off. Returns an id for {@link clearTimeout}. No permission.
 *
 * @example
 * const pause = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));
 * for (const url of feeds) { await fetchFeed(url); await pause(500); } // be polite to the host
 */
declare function setTimeout(callback: () => void, ms?: number): number;

/**
 * Cancels a timer set with {@link setTimeout}; unknown ids are ignored. No permission.
 *
 * @example
 * const timer = setTimeout(() => console.log("slow"), 2000);
 * await work();
 * clearTimeout(timer);
 */
declare function clearTimeout(id: number | undefined): void;

/**
 * A parsed URL, as on the web (WHATWG URL). No permission.
 *
 * @example
 * const url = new URL("https://export.arxiv.org/api/query");
 * url.searchParams.set("id_list", ids.join(","));
 * const res = await zk.fetch(url.href);
 */
declare class URL {
  /** Throws a `TypeError` if `url` (resolved against `base`) isn't a valid URL. */
  constructor(url: string, base?: string | URL);
  /** Whether `new URL(url, base)` would succeed. */
  static canParse(url: string, base?: string | URL): boolean;
  href: string;
  readonly origin: string;
  protocol: string;
  username: string;
  password: string;
  host: string;
  hostname: string;
  port: string;
  pathname: string;
  search: string;
  readonly searchParams: URLSearchParams;
  hash: string;
  toString(): string;
  toJSON(): string;
}

/**
 * A URL's query string, as on the web. No permission.
 *
 * @example
 * const q = new URLSearchParams({ search_query: "cat:math.PR", max_results: "50" });
 * await zk.fetch(`https://export.arxiv.org/api/query?${q}`);
 */
declare class URLSearchParams implements Iterable<[string, string]> {
  constructor(init?: string | Record<string, string> | [string, string][]);
  readonly size: number;
  append(name: string, value: string): void;
  delete(name: string, value?: string): void;
  get(name: string): string | null;
  getAll(name: string): string[];
  has(name: string, value?: string): boolean;
  set(name: string, value: string): void;
  sort(): void;
  forEach(callback: (value: string, name: string, params: URLSearchParams) => void): void;
  keys(): IterableIterator<string>;
  values(): IterableIterator<string>;
  entries(): IterableIterator<[string, string]>;
  [Symbol.iterator](): IterableIterator<[string, string]>;
  /** The query string, without the leading `?`. */
  toString(): string;
}

/**
 * Encodes strings as UTF-8 bytes, as on the web. No permission.
 *
 * @example
 * const bytes = new TextEncoder().encode(text).length; // size of a request body
 */
declare class TextEncoder {
  constructor();
  /** Always `"utf-8"`. */
  readonly encoding: "utf-8";
  encode(input?: string): Uint8Array;
}

/**
 * Decodes UTF-8 bytes to a string, as on the web. Only UTF-8 is supported. No permission.
 *
 * @example
 * const text = new TextDecoder().decode(bytes);
 */
declare class TextDecoder {
  /** `fatal: true` throws a `TypeError` on invalid bytes instead of inserting U+FFFD. */
  constructor(label?: "utf-8" | "utf8", options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: "utf-8";
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  decode(input?: ArrayBuffer | ArrayBufferView, options?: { stream?: boolean }): string;
}

declare namespace zk {
  // ───────────────────────────── Plugin, settings, log ─────────────────────────────

  /**
   * This plugin, as installed. No permission.
   *
   * @example
   * console.log(`${zk.plugin.id} ${zk.plugin.version}, data in ${zk.plugin.dataDir}`);
   */
  const plugin: {
    /** The plugin's id from `plugin.json`; also its folder name. */
    readonly id: string;
    /** The plugin's version from `plugin.json`. */
    readonly version: string;
    /**
     * Where its `data/` folder is on the device (e.g. `/mnt/us/zotero/plugins/<id>/data`). For
     * telling the user where an export went; files are read and written with {@link zk.files}.
     */
    readonly dataDir: string;
  };

  /**
   * The user's settings for this plugin: the parsed `settings.json` next to `plugin.json`, or `{}`
   * if there is none. Read-only; the user edits the file over USB. Read when the plugin loads.
   * No permission.
   *
   * To type your settings, add their fields to {@link zk.Settings}.
   *
   * @example
   * const apiKey = zk.settings.apiKey;
   * if (typeof apiKey !== "string") throw new Error('Put your key in settings.json: { "apiKey": "…" }');
   */
  const settings: Readonly<Settings>;

  /**
   * The shape of `settings.json`. Untyped by default; a plugin describes its own by merging fields
   * into this interface.
   *
   * @example
   * // main.ts
   * declare global {
   *   namespace zk {
   *     interface Settings { apiKey?: string; language?: string }
   *   }
   * }
   * const lang = zk.settings.language ?? "English"; // string
   */
  interface Settings {
    [key: string]: Json | undefined;
  }

  // ───────────────────────────── Storage and files ─────────────────────────────

  /**
   * A private key-value store, kept in the plugin's own folder. Values are JSON (stored as with
   * `JSON.stringify`); what `get` and `update` hand back is a fresh copy, so changing it changes
   * nothing until it is stored. Survives app restarts and updates; emptied by Settings → Plugins → *Reset
   * its data*. No permission.
   *
   * @example
   * await zk.storage.set("lastRun", new Date().toISOString());
   * const seen = await zk.storage.update<string[]>("seen", (seen = []) => [...seen, entry.id]);
   */
  const storage: Storage;

  /** The type of {@link zk.storage}. */
  interface Storage {
    /**
     * The value stored under `key`, or `undefined` if there is none. `T` is what you stored; it is
     * not checked.
     *
     * @example
     * const lastRun = await zk.storage.get<string>("lastRun"); // string | undefined
     */
    get<T = Json>(key: string): Promise<T | undefined>;

    /**
     * Stores a JSON value under `key`, replacing what was there. Queued with the plugin's other
     * storage writes, so writes apply in the order they were asked (see {@link update}).
     *
     * @example
     * await zk.storage.set("lastRun", new Date().toISOString());
     */
    set(key: string, value: unknown): Promise<void>;

    /**
     * Reads, changes and writes `key` as one step: the way to do a read-modify-write. `fn` gets
     * the stored value (`undefined` if there is none) and returns the new one; returning
     * `undefined` deletes the key. Resolves with what was stored.
     *
     * All of a plugin's storage writes — `set`, `delete` and `update` — go through one queue (the
     * app keeps one per plugin) and apply in the order they were asked, one at a time: an
     * update's `fn` starts only when the writes asked before it have finished, however calls
     * interleave. So two event handlers updating the same key can't lose each other's change.
     * `fn` may be `async` and may await other calls (the library, files, `zk.zotero`) — the
     * writes after it wait, so keep it short. If `fn` throws, or its call is stopped by a limit,
     * nothing is stored, the update rejects, and the next write runs.
     *
     * Inside `fn`, write only by returning: calling `update`, `set` or `delete` there (on any
     * key) would wait for `fn` itself, so it rejects at once with an `Error`
     * ("zk.storage.update can't be called inside another update's function", or `set` /
     * `delete` for those). `get` and `keys` are fine.
     *
     * @example
     * // count, safely from any number of handlers at once
     * await zk.storage.update<number>("finished", (n = 0) => n + 1);
     *
     * @example
     * // file a card and remember it, as one step
     * const deck = await zk.storage.update<Card[]>("deck", async (deck = []) => {
     *   const [card, ...rest] = deck;
     *   if (card) await zk.zotero.createItem(fieldsOf(card));
     *   return rest;
     * });
     */
    update<T = Json>(key: string, fn: (old: T | undefined) => T | undefined | Promise<T | undefined>): Promise<T | undefined>;

    /**
     * Removes `key`. Resolves `true` if it was there. Queued with the plugin's other storage
     * writes (see {@link update}).
     *
     * @example
     * await zk.storage.delete("draft");
     */
    delete(key: string): Promise<boolean>;

    /**
     * All keys, in no particular order.
     *
     * @example
     * for (const key of await zk.storage.keys()) if (key.startsWith("feed:")) console.log(key);
     */
    keys(): Promise<string[]>;
  }

  /**
   * Text files in the plugin's own `data/` folder, for exports the user copies off over USB.
   * Paths are relative to `data/` and use `/`; a path that leaves the folder (`..`, absolute
   * paths) is refused. No permission.
   *
   * @example
   * await zk.files.write("highlights/Gaussian random fields.md", markdown);
   * console.log("written to", `${zk.plugin.dataDir}/highlights/`);
   */
  namespace files {
    /**
     * The file's text (UTF-8). Rejects if it doesn't exist.
     *
     * @example
     * const template = await zk.files.read("template.md");
     */
    function read(path: string): Promise<string>;

    /**
     * Writes text (UTF-8), replacing the file and creating folders on the way.
     *
     * @example
     * await zk.files.write("export.md", "# My highlights\n");
     */
    function write(path: string, text: string): Promise<void>;

    /**
     * The names of the entries in a folder (default: `data/` itself); folders end with `/`.
     * Resolves `[]` for a folder that doesn't exist.
     *
     * @example
     * const exports = (await zk.files.list("highlights")).filter((n) => n.endsWith(".md"));
     */
    function list(dir?: string): Promise<string[]>;

    /**
     * Removes a file, or a folder with everything in it. Resolves `true` if it was there.
     *
     * @example
     * await zk.files.remove("highlights/old.md");
     */
    function remove(path: string): Promise<boolean>;
  }

  // ───────────────────────────── Network ─────────────────────────────

  /**
   * Fetches a URL, like the web's `fetch`, but HTTPS only, to declared hosts only, without cookies
   * and without any of the app's credentials. Responses are capped at 10 MB and requests at 30 s
   * each (less with {@link RequestInit.timeout}); going over rejects with a {@link zk.LimitError}
   * (`"responseSize"`, `"responseTime"`).
   * `zotero.org`, every `*.zotero.org` host and the user's WebDAV host are always refused, even
   * with `net:*`.
   *
   * **Permission**: `net:<host>` for the URL's host (e.g. `"net:export.arxiv.org"`; `"net:*.example.org"`
   * covers subdomains), or `net:*` for any host. Redirects are checked the same way.
   *
   * Network errors reject with an `Error`; HTTP errors don't reject: check `ok` or `status`.
   *
   * @example
   * const res = await zk.fetch("https://export.arxiv.org/rss/math.PR");
   * if (!res.ok) throw new Error(`arXiv: HTTP ${res.status}`);
   * const xml = await res.text();
   *
   * @example
   * const res = await zk.fetch("https://api.example.org/v1/explain", {
   *   method: "POST",
   *   headers: { "content-type": "application/json", authorization: `Bearer ${key}` },
   *   body: JSON.stringify({ text }),
   * });
   * const { answer } = await res.json<{ answer: string }>();
   */
  function fetch(url: string, init?: RequestInit): Promise<Response>;

  /** Options for {@link zk.fetch}. */
  interface RequestInit {
    /** Default `"GET"`. */
    method?: "GET" | "HEAD" | "POST" | "PUT" | "PATCH" | "DELETE";
    /**
     * Request headers, as an object or as `[name, value]` pairs (for repeated names). `Cookie` is
     * not allowed.
     */
    headers?: Record<string, string> | [string, string][];
    /** Request body; send JSON with `JSON.stringify` and a `content-type` header. */
    body?: string;
    /**
     * Give up after this many milliseconds (at most, and by default, 30 000), rejecting with a
     * {@link zk.LimitError} `"responseTime"`. For calls that share a budget, e.g. several feeds
     * fetched in one 30-second background run.
     *
     * @example
     * const res = await zk.fetch(feedUrl, { timeout: 8000 });
     */
    timeout?: number;
  }

  /**
   * Response headers: a read-only subset of the web's `Headers`. Names are case-insensitive;
   * repeated headers are joined with `", "`.
   *
   * @example
   * const type = res.headers.get("Content-Type") ?? "";
   * for (const [name, value] of res.headers) console.log(name, value); // names in lower case
   */
  interface Headers extends Iterable<[string, string]> {
    /** The header's value, or `null` if it is absent. */
    get(name: string): string | null;
    /** Whether the header is present. */
    has(name: string): boolean;
    /** `[name, value]` pairs, names in lower case, sorted by name. */
    entries(): IterableIterator<[string, string]>;
    [Symbol.iterator](): IterableIterator<[string, string]>;
  }

  /** The answer to {@link zk.fetch}. The body is read whole (it is capped), and can be read again. */
  interface Response {
    /** `true` for status 200–299. */
    readonly ok: boolean;
    /** The HTTP status, e.g. `200`, `404`. */
    readonly status: number;
    /** The URL answered, after redirects. */
    readonly url: string;
    /** Response headers, as in the web's `Headers`: `res.headers.get("content-type")`. */
    readonly headers: Headers;
    /**
     * The body as text, decoded as UTF-8 whatever the response's charset (as on the web); invalid
     * bytes become U+FFFD. For another encoding, decode {@link bytes} yourself.
     */
    text(): Promise<string>;
    /**
     * The body as bytes.
     *
     * @example
     * // a Latin-1 feed
     * const xml = Array.from(await res.bytes(), (b) => String.fromCharCode(b)).join("");
     */
    bytes(): Promise<Uint8Array>;
    /** The body parsed as JSON. `T` is not checked. Rejects if it isn't JSON. */
    json<T = Json>(): Promise<T>;
  }

  // ───────────────────────────── Reading the library ─────────────────────────────

  /**
   * Reads the library as it is on the Kindle (synced from Zotero, plus changes not sent yet).
   * Everything comes back as plain objects; nothing is live.
   */
  namespace library {
    /**
     * Every item matching `query` (all items if none), in the library's order, fetched lazily —
     * so a whole library can be walked without loading it at once. Filters combine (AND). To find
     * items by words (title, creators…), use {@link search}.
     *
     * **Permission**: `library:read`.
     *
     * @example
     * for await (const item of zk.library.items({ collection: inbox.key })) {
     *   if (!item.read) console.log(item.title);
     * }
     *
     * @example
     * // first ten items tagged "to-read"
     * for await (const item of zk.library.items({ tag: "to-read", limit: 10 })) console.log(item.title);
     */
    function items(query?: ItemsQuery): AsyncIterable<Item>;

    /**
     * One item by its key, or `null` if there is none (or it is in the trash).
     *
     * **Permission**: `library:read`.
     *
     * @example
     * const item = await zk.library.item(annotation.itemKey);
     */
    function item(key: string): Promise<Item | null>;

    /**
     * The items with a DOI, arXiv id or ISBN — the way to ask "is this paper already in my
     * library?". DOIs match case-insensitively and with or without a `https://doi.org/` prefix;
     * arXiv ids match whatever the version suffix (`2401.01234v3` finds `2401.01234v1`); ISBNs
     * match as ISBN-10 or ISBN-13. Several identifiers match any of them. Usually zero or one item;
     * more if the library has duplicates.
     *
     * **Permission**: `library:read`.
     *
     * @example
     * const [existing] = await zk.library.findByIdentifier({ arxiv: "2401.01234v2" });
     * if (existing) console.log("already have", existing.title);
     */
    function findByIdentifier(id: { doi?: string; arxiv?: string; isbn?: string }): Promise<Item[]>;

    /**
     * All collections, each with its full path, sorted by path.
     *
     * **Permission**: `library:read`.
     *
     * @example
     * const inbox = (await zk.library.collections()).find((c) => c.path.join("/") === "Reading/Inbox");
     */
    function collections(): Promise<Collection[]>;

    /**
     * All tags in the library, by name.
     *
     * **Permission**: `library:read`.
     *
     * @example
     * const names = (await zk.library.tags()).map((t) => t.name);
     */
    function tags(): Promise<Tag[]>;

    /**
     * Finds items by words, like the app's search box: matches title, creators, year, publication
     * and tags; best matches first. The one way to search; {@link items} lists by collection or tag.
     *
     * **Permission**: `library:read`.
     *
     * @example
     * const [best] = await zk.library.search("gaussian random fields adler");
     */
    function search(text: string): Promise<Item[]>;

    /**
     * An item's attachments (PDFs, EPUBs, …), whether downloaded to the Kindle or not.
     *
     * **Permission**: `library:read`.
     *
     * @example
     * const pdf = (await zk.library.attachments(item.key)).find((a) => a.downloaded);
     */
    function attachments(itemKey: string): Promise<Attachment[]>;

    /**
     * An attachment's annotations (highlights, notes, …) in reading order.
     *
     * **Permission**: `annotations:read`.
     *
     * @example
     * const highlights = (await zk.library.annotations(pdf.key)).filter((a) => a.type === "highlight");
     */
    function annotations(attachmentKey: string): Promise<Annotation[]>;

    /**
     * Annotations across the library, newest change first, fetched lazily. `since` (an ISO date)
     * stops at annotations last changed before it.
     *
     * **Permission**: `annotations:read`.
     *
     * @example
     * const lastExport = await zk.storage.get<string>("lastExport");
     * for await (const a of zk.library.recentAnnotations({ since: lastExport })) touched.add(a.itemKey);
     */
    function recentAnnotations(query?: { since?: string; limit?: number }): AsyncIterable<Annotation>;

    /**
     * An item's child notes (Zotero note items), as HTML, oldest first.
     *
     * **Permission**: `notes:read`.
     *
     * @example
     * for (const note of await zk.library.notes(old.key)) await zk.zotero.addNote(replacement.key, note.html);
     */
    function notes(itemKey: string): Promise<Note[]>;

    /**
     * The text of a downloaded book or paper (PDF, EPUB, …), extracted by the app's own engines.
     * Rejects if the attachment isn't downloaded to the Kindle (see {@link Attachment.downloaded})
     * or isn't a document. Can take a few seconds for a long book.
     *
     * **Permission**: `fulltext:read`.
     *
     * @example
     * const text = await zk.library.fullText(pdf.key);
     * const words = text.split(/\s+/).length;
     */
    function fullText(attachmentKey: string): Promise<string>;
  }

  /** Filters for {@link zk.library.items}. They combine: an item must match all that are given. */
  interface ItemsQuery {
    /** Only items in this collection (its key; not its subcollections). */
    collection?: string;
    /** Only items with this tag (its name, exact). */
    tag?: string;
    /** Stop after this many items. */
    limit?: number;
  }

  // ───────────────────────────── Changing the library ─────────────────────────────

  /**
   * Asks the app to change the Zotero library. Plugins never write to Zotero themselves: the app
   * records each change, shows it in the library at once, and the sync sends it to Zotero later
   * (without ever overwriting an edit made elsewhere). Settings → Plugins → *What it changed*
   * lists every change a plugin asked for and what became of it.
   *
   * Every call changes one thing and resolves with a {@link Change}: the key of what changed and a
   * `changeId` to {@link cancel} it while it is still unsent.
   *
   * - **Adding** (items, collections, tags, notes) is allowed by its permission and resolves at once.
   * - **Removing** (trash, remove from a collection, remove a tag, delete a note) needs the user's
   *   yes every time: the app shows its own confirmation and the call resolves with the change
   *   (done) or `null` (declined, or not allowed right now) — so `if (await zk.zotero.trash(key))`
   *   reads as "if it was trashed". Removals are only asked in answer to something the user just
   *   did (an action, a screen, an event they caused on the Kindle); from a background task or an
   *   event that came in with a sync they resolve `null` without asking. Removals asked together
   *   (before awaiting any of them, e.g. with `Promise.all`) are confirmed together, in one list,
   *   with one answer for all. Nothing is ever deleted for good except a note this plugin created:
   *   items go to Zotero's trash.
   * - **Editing** existing data (a title, a note, an annotation) is not possible in API v0.
   */
  namespace zotero {
    /**
     * Creates an item with the given metadata, optionally filed into collections and tagged.
     * One change: cancelling it withdraws the item with its collections and tags. The new item
     * is readable at once (`zk.library.item(change.key)`).
     *
     * **Permission**: `zotero:create-items`; also `zotero:collections` if `collections` isn't
     * empty, `zotero:tags` if `tags` isn't empty.
     *
     * @example
     * const change = await zk.zotero.createItem(
     *   {
     *     type: "preprint",
     *     title: entry.title,
     *     creators: entry.authors.map((name) => ({ type: "author", name })),
     *     date: entry.date,
     *     arxiv: entry.id,
     *     url: `https://arxiv.org/abs/${entry.id}`,
     *     abstract: entry.summary,
     *   },
     *   { collections: [inbox.key], tags: ["feed-riffle"] },
     * );
     * await zk.storage.set(`undo:${entry.id}`, change.changeId);
     */
    function createItem(fields: ItemFields, opts?: { collections?: string[]; tags?: string[] }): Promise<Change>;

    /**
     * Files an item into a collection (it stays in its other collections). `change.key` is the
     * item's key.
     *
     * **Permission**: `zotero:collections`.
     *
     * @example
     * await zk.zotero.addToCollection(item.key, inbox.key);
     */
    function addToCollection(itemKey: string, collectionKey: string): Promise<Change>;

    /**
     * Adds a tag to an item (nothing changes if it already has it). `change.key` is the item's key.
     *
     * **Permission**: `zotero:tags`.
     *
     * @example
     * await zk.zotero.addTag(item.key, "finished-on-kindle");
     */
    function addTag(itemKey: string, tag: string): Promise<Change>;

    /**
     * Adds a child note to an item. `html` is Zotero note HTML (paragraphs, headings, lists,
     * links, emphasis). `change.key` is the new note's key.
     *
     * **Permission**: `zotero:notes`.
     *
     * @example
     * await zk.zotero.addNote(item.key, `<p>Finished on ${new Date().toDateString()}.</p>`);
     */
    function addNote(itemKey: string, html: string): Promise<Change>;

    /**
     * Asks the user to move an item to Zotero's trash (recoverable there). Resolves with the
     * change if they said yes (`change.key` is the item's key), `null` if not. To trash several,
     * ask for them together: one confirmation lists them all.
     *
     * **Permission**: `zotero:remove`, and the user's yes each time.
     *
     * @example
     * // replace an old version with a new one, keeping its notes
     * for (const note of await zk.library.notes(old.key)) await zk.zotero.addNote(fresh.key, note.html);
     * if (!(await zk.zotero.trash(old.key))) console.log("kept the old version");
     *
     * @example
     * // several at once, in one confirmation
     * const trashed = await Promise.all(duplicates.map((d) => zk.zotero.trash(d.key)));
     */
    function trash(itemKey: string): Promise<Change | null>;

    /**
     * Asks the user to take an item out of a collection (the item stays in the library).
     *
     * **Permission**: `zotero:remove`, and the user's yes each time.
     *
     * @example
     * const removed = await zk.zotero.removeFromCollection(item.key, inbox.key); // Change | null
     */
    function removeFromCollection(itemKey: string, collectionKey: string): Promise<Change | null>;

    /**
     * Asks the user to remove a tag from an item.
     *
     * **Permission**: `zotero:remove`, and the user's yes each time.
     *
     * @example
     * await zk.zotero.removeTag(item.key, "to-read");
     */
    function removeTag(itemKey: string, tag: string): Promise<Change | null>;

    /**
     * Asks the user to delete a note — only one this plugin created (see {@link Note.createdBy});
     * any other note is refused. The one hard delete in the API.
     *
     * **Permission**: `zotero:remove`, and the user's yes each time.
     *
     * @example
     * const mine = (await zk.library.notes(item.key)).filter((n) => n.createdBy === zk.plugin.id);
     * for (const note of mine) await zk.zotero.deleteNote(note.key);
     */
    function deleteNote(noteKey: string): Promise<Change | null>;

    /**
     * Withdraws one of this plugin's changes that hasn't been sent to Zotero yet, of any kind — no
     * confirmation, since nothing leaves Zotero. Withdrawing an add takes it back (withdrawing a
     * new item also withdraws what was added to it); withdrawing a removal the user agreed to puts
     * back what it removed (an item comes back out of the trash, a deleted note comes back).
     * Resolves `true` if withdrawn, `false` if the change was already sent or isn't this plugin's.
     * Undoing a sent add is a removal: ask with {@link trash} and friends. Undoing a sent removal
     * isn't possible from a plugin (restoring is an edit); the user restores it in Zotero.
     *
     * **Permission**: none beyond the one the change needed.
     *
     * @example
     * const changeId = await zk.storage.get<string>(`undo:${entry.id}`);
     * if (changeId && !(await zk.zotero.cancel(changeId))) {
     *   await zk.zotero.trash(itemKey); // already synced: ask instead
     * }
     */
    function cancel(changeId: string): Promise<boolean>;

    /**
     * This plugin's changes not sent to Zotero yet, oldest first.
     *
     * **Permission**: none.
     *
     * @example
     * const unsent = await zk.zotero.pending();
     * zk.setBadge(unsent.length ? `${unsent.length} unsent` : null);
     */
    function pending(): Promise<PendingChange[]>;
  }

  /** What a change asked of {@link zk.zotero} resolves with (a removal: if the user said yes). */
  interface Change {
    /**
     * The key of what the change made or changed: the new item or note; the item filed, tagged,
     * trashed, or taken out of a collection or tag; the note deleted.
     */
    key: string;
    /** Identifies the change, to {@link zk.zotero.cancel} it while it is unsent. */
    changeId: string;
  }

  /** A change of this plugin's that hasn't been sent to Zotero yet ({@link zk.zotero.pending}). */
  type PendingChange = {
    /** As returned by the call that asked for it; for {@link zk.zotero.cancel}. */
    changeId: string;
    /** When it was asked for (ISO 8601). */
    requested: string;
  } & (
    | {
        kind: "createItem";
        /** The new item. */
        key: string;
        fields: ItemFields;
        /** Collections it was filed into with it (`[]` if none). */
        collections: string[];
        /** Tags it was given with it (`[]` if none). */
        tags: string[];
      }
    | { kind: "addToCollection"; /** The item. */ key: string; collection: string }
    | { kind: "addTag"; /** The item. */ key: string; tag: string }
    | { kind: "addNote"; /** The new note. */ key: string; itemKey: string }
    | { kind: "trash"; /** The item. */ key: string }
    | { kind: "removeFromCollection"; /** The item. */ key: string; collection: string }
    | { kind: "removeTag"; /** The item. */ key: string; tag: string }
    | { kind: "deleteNote"; /** The note. */ key: string }
  );

  // ───────────────────────────── Events ─────────────────────────────

  /**
   * Runs `handler` whenever `event` happens. Register handlers when the plugin loads (at the top
   * level of `main.js`). A handler has 5 seconds from when it starts running (not counting time
   * queued behind other plugins or waiting for the user's confirmation); going over stops it
   * (see {@link LimitError}: what it started is cancelled). An error it throws is logged with
   * its stack.
   *
   * Every event says where the change was made: `origin: "kindle"` (the user did it on this
   * Kindle, one event per change, as it happens) or `origin: "zotero"` (it came in with a sync:
   * one event per kind per sync, listing everything). So events that can arrive with a sync carry
   * a list — `annotations` or `items` — with one entry when the change was made on the Kindle.
   * The first full sync of a library fires only `syncFinished` (with `initial: true`).
   *
   * **Permission**: what the event carries — `annotations:read` for `highlight…` events,
   * `library:read` for `book…` and `itemMarked…` events; `syncFinished` needs none.
   *
   * @example
   * zk.on("highlightCreated", async ({ origin, annotations }) => {
   *   for (const a of annotations) console.log(origin, a.text);
   * });
   *
   * @example
   * zk.on("syncFinished", ({ initial }) => {
   *   if (!initial) console.log("synced");
   * });
   */
  function on<K extends keyof Events>(event: K, handler: (e: Events[K]) => void | Promise<void>): void;

  /** Every event {@link zk.on} can listen to, and what its handler receives. */
  interface Events {
    /** Highlights or other annotations were made. */
    highlightCreated: AnnotationsEvent;
    /** Annotations were changed (comment, colour, text). */
    highlightChanged: AnnotationsEvent;
    /** Annotations were deleted; only their keys are left. */
    highlightDeleted: {
      origin: Origin;
      annotations: Pick<Annotation, "key" | "attachmentKey" | "itemKey">[];
    };
    /** A book or paper was opened on the Kindle. */
    bookOpened: BookEvent;
    /** The user turned past the last page of a book or paper. */
    bookFinished: BookEvent;
    /** Items were marked read. */
    itemMarkedRead: ItemsEvent;
    /** Items were marked unread. */
    itemMarkedUnread: ItemsEvent;
    /** A sync with Zotero finished successfully (fires after the other events of that sync). */
    syncFinished: {
      origin: "zotero";
      /** `true` for the first full sync of a library, which fires no other events. */
      initial: boolean;
    };
  }

  /** Where a change was made: on this Kindle, or elsewhere and brought in by a sync. */
  type Origin = "kindle" | "zotero";

  /** `highlightCreated`, `highlightChanged`. */
  interface AnnotationsEvent {
    origin: Origin;
    /** The annotations, as {@link zk.library.annotations} returns them. One when `origin` is `"kindle"`. */
    annotations: Annotation[];
  }

  /** `itemMarkedRead`, `itemMarkedUnread`. */
  interface ItemsEvent {
    origin: Origin;
    /** The items, after the change. One when `origin` is `"kindle"`. */
    items: Item[];
  }

  /** `bookOpened`, `bookFinished`: these only happen on the Kindle. */
  interface BookEvent {
    origin: "kindle";
    item: Item;
    /** The file that was read. */
    attachment: Attachment;
  }

  // ───────────────────────────── Background work ─────────────────────────────

  /**
   * Registers the plugin's background task (one per plugin; registering again replaces it). The
   * app runs it after a successful sync, at most once an hour, while the Kindle is awake and
   * online; it never wakes a sleeping Kindle. A run has 30 seconds from when it starts running;
   * going over stops it (see {@link LimitError}: what it started is cancelled). Three failed runs
   * in a row
   * turn the plugin off. A background task can't show anything or ask the user: removals resolve
   * `null`; use {@link setBadge} to say there is something to see.
   *
   * **Permission**: `background`.
   *
   * @example
   * zk.background(async () => {
   *   const fresh = await refreshFeeds();
   *   zk.setBadge(fresh ? `${fresh} new` : null);
   * });
   */
  function background(task: () => void | Promise<void>): void;

  /**
   * Sets a short badge on the plugin, shown wherever the app lists it (e.g. `"42 new"`); `null`
   * clears it. Kept until changed, across restarts. Long text is cut. No permission.
   *
   * @example
   * zk.setBadge("3 new");
   */
  function setBadge(text: string | null): void;

  // ───────────────────────────── Entry points (part 2: not shown yet) ─────────────────────────────

  /**
   * Adds an action to the selection bar (what appears when the user selects text in a book).
   *
   * **Part 2: not shown yet.** In API v0 the action is checked and recorded but doesn't appear;
   * plugins written now keep working when it does.
   *
   * `run` has at least 30 seconds (the fetch cap), counted from when it starts running; time
   * waiting for the user's confirmation doesn't count.
   *
   * No permission: the selection and its item are handed over because the user chose the action.
   *
   * @example
   * zk.selectionAction({
   *   id: "explain",
   *   title: "Explain",
   *   run: async ({ text, item }) => {
   *     const answer = await explain(text, item.title);
   *     return answer; // shown in the result pop-up
   *   },
   * });
   */
  function selectionAction(action: Action<TextSelection>): void;

  /**
   * Adds an action to an item's *More…* menu.
   *
   * **Part 2: not shown yet.** In API v0 the action is checked and recorded but doesn't appear.
   *
   * `run` has at least 30 seconds (the fetch cap), counted from when it starts running; time
   * waiting for the user's confirmation doesn't count.
   *
   * No permission: the item is handed over because the user chose the action.
   *
   * @example
   * zk.itemAction({
   *   id: "export",
   *   title: "Export highlights",
   *   run: async (item) => `Written to ${await exportHighlights(item)}`,
   * });
   */
  function itemAction(action: Action<Item>): void;

  /**
   * Registers a screen of the plugin's own.
   *
   * **Part 2: not shown yet, and provisional.** Part 2 designs screens and the building blocks
   * they are made of; `render`'s argument and result will be typed then. In API v0 the screen is
   * recorded but can't be opened.
   *
   * @example
   * zk.screen("cards", () => ({}));
   */
  function screen(id: string, render: (params: ScreenParams) => ScreenContent | Promise<ScreenContent>): void;

  /** An action the user starts ({@link selectionAction}, {@link itemAction}). */
  interface Action<T> {
    /** Unique within the plugin: lowercase letters, digits and dashes. */
    id: string;
    /** What the user sees, short (one or two words). */
    title: string;
    /**
     * Does the work. A returned string is shown to the user in the result pop-up (part 2);
     * a thrown error is shown as the plugin having failed.
     */
    run(target: T): ActionResult | Promise<ActionResult>;
  }

  /** What an action's `run` may return: text for the result pop-up, or nothing. */
  type ActionResult = string | void;

  /** What {@link selectionAction} hands to `run`. */
  interface TextSelection {
    /** The selected text. */
    text: string;
    /** The item being read. */
    item: Item;
    /** The file being read. */
    attachment: Attachment;
    /** The page label where the selection starts, if the document has pages. */
    pageLabel: string | null;
  }

  /** **Provisional (part 2).** What a screen is opened with. */
  type ScreenParams = Readonly<Record<string, Json>>;

  /** **Provisional (part 2)**: what `render` returns; part 2 defines it. */
  type ScreenContent = unknown;

  // ───────────────────────────── Data ─────────────────────────────

  /**
   * A library item (a paper, book, …), the same shape wherever it comes from. Fields Zotero leaves
   * empty are `null`; lists are empty arrays.
   */
  interface Item {
    /** Zotero's key, e.g. `"ABCD2345"`. */
    key: string;
    /** Zotero's item type. */
    type: ItemType;
    /** The title; `""` if it has none. */
    title: string;
    /** Authors, editors, … in order. */
    creators: Creator[];
    /** The year, parsed from `date`. */
    year: number | null;
    /** The date as entered in Zotero (free-form, e.g. `"2024-01-03"` or `"March 2019"`). */
    date: string | null;
    /** Journal, book, proceedings or website title. */
    publication: string | null;
    /** The DOI, without a `https://doi.org/` prefix. */
    doi: string | null;
    /** The arXiv id, without a version suffix (`"2401.01234"`). */
    arxiv: string | null;
    isbn: string | null;
    url: string | null;
    abstract: string | null;
    /** Tag names. */
    tags: string[];
    /** Keys of the collections it is in. */
    collections: string[];
    /** Whether it is marked read. */
    read: boolean;
    /** Zotero's *Extra* field. */
    extra: string | null;
    /**
     * The citation key: Zotero's own `citationKey` field when it has one, else the
     * `Citation Key:` line Better BibTeX writes into *Extra*.
     */
    citationKey: string | null;
  }

  /** Zotero item types. Others exist; these are the common ones. */
  type ItemType =
    | "journalArticle"
    | "preprint"
    | "book"
    | "bookSection"
    | "conferencePaper"
    | "thesis"
    | "report"
    | "manuscript"
    | "webpage"
    | "document"
    | (string & {});

  /** A creator of an item: a person (`first`, `last`) or an organisation (`name`). */
  interface Creator {
    /** `"author"`, `"editor"`, … */
    type: "author" | "editor" | "contributor" | "translator" | (string & {});
    first?: string;
    last?: string;
    /** A single-field name, for organisations or names that don't split. */
    name?: string;
  }

  /** The metadata {@link zk.zotero.createItem} takes. `type` and `title` are required. */
  interface ItemFields {
    type: ItemType;
    title: string;
    creators?: Creator[];
    /** Free-form, as in Zotero; ISO (`"2024-01-03"`) is best. */
    date?: string;
    publication?: string;
    doi?: string;
    /** The arXiv id (a version suffix is kept as given). */
    arxiv?: string;
    isbn?: string;
    url?: string;
    abstract?: string;
    extra?: string;
  }

  /** A collection. */
  interface Collection {
    key: string;
    name: string;
    /** The parent collection's key; `null` at the top level. */
    parent: string | null;
    /** Names from the top level down, ending with its own: `["Reading", "arXiv"]`. */
    path: string[];
  }

  /** A tag in the library. */
  interface Tag {
    name: string;
    /** How many items have it. */
    count: number;
  }

  /** A file attached to an item. */
  interface Attachment {
    key: string;
    /** The item it belongs to. */
    itemKey: string;
    title: string;
    /** E.g. `"application/pdf"`, `"application/epub+zip"`. */
    contentType: string;
    filename: string | null;
    /** Whether the file is on the Kindle (needed for {@link zk.library.fullText}). */
    downloaded: boolean;
  }

  /** An annotation in a book or paper. */
  interface Annotation {
    key: string;
    /** The file it is in. */
    attachmentKey: string;
    /** The item that file belongs to. */
    itemKey: string;
    type: "highlight" | "underline" | "note" | "image" | "ink";
    /** The highlighted text (`null` for notes and images). */
    text: string | null;
    /** The user's comment. */
    comment: string | null;
    /** As in Zotero, e.g. `"#ffd400"`. */
    color: string;
    /** The page label shown in the document, e.g. `"12"` or `"xii"`. */
    pageLabel: string | null;
    tags: string[];
    /** ISO 8601. */
    created: string;
    /** ISO 8601. */
    modified: string;
  }

  /** A Zotero note attached to an item. */
  interface Note {
    key: string;
    /** The item it belongs to. */
    itemKey: string;
    /** The note's HTML, as Zotero stores it. */
    html: string;
    /** The id of the plugin that created it, or `null` (made in Zotero). */
    createdBy: string | null;
    /** ISO 8601. */
    created: string;
    /** ISO 8601. */
    modified: string;
  }

  /** A JSON value. */
  type Json = string | number | boolean | null | Json[] | { [key: string]: Json };

  // ───────────────────────────── Permissions and errors ─────────────────────────────

  /** A permission, as written in `plugin.json`. */
  type Permission =
    | "library:read"
    | "annotations:read"
    | "fulltext:read"
    | "notes:read"
    | `net:${string}`
    | "zotero:create-items"
    | "zotero:collections"
    | "zotero:tags"
    | "zotero:notes"
    | "zotero:remove"
    | "background";

  /**
   * Thrown (or rejected with) when the plugin calls something its `plugin.json` doesn't declare.
   * The message says what to add, e.g.
   * `zk.fetch("https://api.deepl.com/…") needs "net:api.deepl.com" in plugin.json`.
   *
   * @example
   * try {
   *   await zk.library.fullText(pdf.key);
   * } catch (e) {
   *   if (e instanceof zk.PermissionError) console.log("missing", e.permission);
   *   else throw e;
   * }
   */
  class PermissionError extends Error {
    /** Only the app makes these. */
    private constructor();
    readonly name: "PermissionError";
    /** The permission that was missing, exactly as it must appear in `plugin.json`. */
    readonly permission: Permission;
  }

  /**
   * Thrown (or rejected with) when the plugin goes over one of the app's limits; the message names
   * it. `time`, `memory` and `stack` stop the running handler or task; the fetch limits only fail
   * that fetch.
   *
   * `time` is the call's budget: event handlers 5 s, the background task 30 s, actions at least
   * 30 s. It counts from when the call starts running; time queued behind other plugins' calls
   * and time waiting for the user to answer a confirmation never count. `memory` is 32 MB for a
   * plugin, and 96 MB for all plugins together: a call whose allocation would pass either is
   * stopped.
   *
   * When a call is stopped, the work it started in the app (fetches, library reads, storage and
   * file operations, timers) is cancelled, and every promise of that work still pending rejects
   * with a `LimitError` — none is left pending forever. So {@link Storage.update}'s queue, or any
   * queue a plugin keeps itself, moves on to the next job instead of hanging. Changes to Zotero
   * already asked for stay asked for (each resolved at once); a removal still waiting for the
   * user's answer is withdrawn. Code that catches the `LimitError` can't start new work for the
   * stopped call: such calls reject at once with the same `LimitError`.
   *
   * After a limit stop that cut into running code (not just a call left waiting too long), the
   * plugin is restarted: `main.js` runs afresh in a new runtime, so module-level variables start
   * over. Keep anything that must survive in {@link zk.storage}. Every stop counts as a failure;
   * three failures in a row turn the plugin off.
   *
   * @example
   * try {
   *   const res = await zk.fetch(bigFeedUrl);
   * } catch (e) {
   *   if (e instanceof zk.LimitError && e.limit === "responseSize") console.log("feed too large");
   *   else throw e;
   * }
   */
  class LimitError extends Error {
    /** Only the app makes these. */
    private constructor();
    readonly name: "LimitError";
    /** Which limit: run time, memory, stack depth, or a fetch's response size or duration. */
    readonly limit: "time" | "memory" | "stack" | "responseSize" | "responseTime";
  }
}
