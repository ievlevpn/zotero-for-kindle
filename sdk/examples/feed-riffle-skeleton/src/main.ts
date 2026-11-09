// Feed Riffle for the Kindle — skeleton: the data flow, no screens yet.
//
// Feed Riffle (the Zotero desktop plugin, ~/Playground/zotero-feed-riffle) deals a feed backlog
// out one card at a time: discard, keep (file into a collection, with tags and a note) or skip,
// with undo. A card whose DOI or arXiv id is already in the library is marked "in library";
// keeping it can replace the old item: its notes are copied to the new one and the old one goes
// to Zotero's trash, with the user's yes.
//
// On the Kindle there is no Zotero feed reader, so the plugin reads the feeds itself:
//
//   settings.json ─ feeds ─▶ background: fetch, parse, mark "in library" ─▶ deck (zk.storage)
//   deck ─▶ discard / skip / keep({ collection, tags, note }) ─▶ zk.zotero.createItem, addNote
//   keep ─▶ replaceOld(old, new): copy notes, ask to trash the old one
//   undo ─▶ zk.zotero.cancel the last keep's changes (or ask, if they were already synced)
//
// The functions a screen would call (discard, skip, keep, keepRecent, replaceOld, undo) are
// exported; the deck screen is a placeholder until part 2 brings screens.

import { filter, isElementNode, parse, type TNode } from "txml/txml";

// ───────────────────────────── Settings ─────────────────────────────

// settings.json, next to plugin.json, edited over USB:
//   { "feeds": ["https://rss.arxiv.org/rss/math.PR", "https://…/atom.xml"], "tags": ["riffled"] }
declare global {
  namespace zk {
    interface Settings {
      /** RSS or Atom feed addresses. */
      feeds?: string[];
      /** Tags every kept item gets, besides the ones chosen when keeping it. */
      tags?: string[];
    }
  }
}

// ───────────────────────────── State ─────────────────────────────

/** A feed entry, as dealt. Plain JSON, so the deck can live in zk.storage. */
interface Card {
  /** `arxiv:<id with version>`, `doi:<doi>`, or the entry's own id: one card per paper version. */
  id: string;
  feed: string;
  title: string;
  authors: string[];
  date: string | null;
  link: string | null;
  summary: string;
  doi: string | null;
  /** With its version suffix, as the feed gives it (`2401.01234v2`). */
  arxiv: string | null;
  /** Keys of library items with the same DOI or arXiv id (any version): the replace candidates. */
  inLibrary: string[];
}

/** One step of the undo history. A keep remembers the changes it asked of Zotero. */
type Step =
  | { kind: "discard" | "skip"; card: Card }
  | { kind: "keep"; card: Card; itemKey: string; changes: { kind: "createItem" | "trash"; key: string; changeId: string }[] };

interface State {
  deck: Card[];
  /** Ids of cards decided (discarded or kept), so a refresh doesn't deal them again. */
  seen: string[];
  undo: Step[];
  /** Collection keys, most recently used first: keys 1–9 file straight into these. */
  recent: string[];
}

const LIMITS = { seen: 5000, undo: 50, recent: 9 };

// Calls interleave at each `await` (a background refresh can run while the user is deciding),
// so every read-modify-write of the state runs through `serially`, one after another. Slow work —
// the network, the user's confirmation — is done outside it. (A call stopped by a limit has what
// it started rejected with zk.LimitError, so a stopped job never blocks the queue.)
let queue: Promise<unknown> = Promise.resolve();
function serially<T>(job: () => Promise<T>): Promise<T> {
  const result = queue.then(job, job);
  queue = result.catch(() => {});
  return result;
}

async function load(): Promise<State> {
  return (await zk.storage.get<State>("state")) ?? { deck: [], seen: [], undo: [], recent: [] };
}

/** Loads the state, lets `change` modify it, saves it, and returns what `change` returned. */
function withState<T>(change: (state: State) => T | Promise<T>): Promise<T> {
  return serially(async () => {
    const state = await load();
    const result = await change(state);
    state.seen = state.seen.slice(-LIMITS.seen);
    state.undo = state.undo.slice(-LIMITS.undo);
    await zk.storage.set("state", state);
    const left = state.deck.length;
    zk.setBadge(left ? `${left} card${left === 1 ? "" : "s"}` : null);
    return result;
  });
}

// ───────────────────────────── Background: fetch, parse, dedupe ─────────────────────────────

// After a sync, at most hourly, with 30 seconds in all: fetch each feed (8 s at most each, so
// one slow host can't eat the run), parse it, drop what is already dealt or decided, mark what
// is already in the library, and add the rest to the deck.
zk.background(async () => {
  const before = await load(); // a snapshot is enough here: the merge below checks again
  const known = new Set([...before.seen, ...before.deck.map((c) => c.id)]);
  const fresh: Card[] = [];
  for (const url of zk.settings.feeds ?? []) {
    try {
      const res = await zk.fetch(url, { timeout: 8000 });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      for (const card of parseFeed(await res.text(), url)) {
        if (known.has(card.id)) continue;
        known.add(card.id);
        card.inLibrary = await inLibrary(card);
        fresh.push(card);
      }
    } catch (e) {
      // One bad feed shouldn't stop the others. (A LimitError "time" — the whole run's 30 s —
      // can't be caught: it ends the run, and the next run starts over.)
      console.warn("skipped feed", url, String(e));
    }
  }
  const added = await withState((s) => {
    const dealt = new Set([...s.seen, ...s.deck.map((c) => c.id)]); // the user may have decided meanwhile
    const add = fresh.filter((c) => !dealt.has(c.id));
    s.deck.push(...add);
    return add.length;
  });
  console.log(`${added} new cards`);
});

/**
 * Library items with the card's DOI or arXiv id. findByIdentifier ignores the arXiv version
 * suffix, so a `v2` announcement finds the `v1` saved months ago.
 */
async function inLibrary(card: Card): Promise<string[]> {
  if (!card.doi && !card.arxiv) return [];
  const items = await zk.library.findByIdentifier({ doi: card.doi ?? undefined, arxiv: card.arxiv ?? undefined });
  return items.map((i) => i.key);
}

// ───────────────────────────── The deck ─────────────────────────────

/** ← Discard: the card is decided and won't come back. */
export function discard(): Promise<Card | undefined> {
  return withState((s) => {
    const card = s.deck.shift();
    if (card) {
      s.seen.push(card.id);
      s.undo.push({ kind: "discard", card });
    }
    return card;
  });
}

/** s Skip: to the back of the deck, for another day. */
export function skip(): Promise<Card | undefined> {
  return withState((s) => {
    const card = s.deck.shift();
    if (card) {
      s.deck.push(card);
      s.undo.push({ kind: "skip", card });
    }
    return card;
  });
}

/**
 * → Keep: creates the item in Zotero, filed into `collection`, tagged, with `note` as a child
 * note. Resolves with the new item's key and the old items the user may want to replace
 * ({@link replaceOld}).
 */
export function keep(opts: { collection?: string; tags?: string[]; note?: string } = {}) {
  return withState(async (s) => {
    const card = s.deck.shift();
    if (!card) return null;

    // One call makes the item, files it and tags it: one change, which `cancel` withdraws whole.
    const tags = [...(zk.settings.tags ?? []), ...(opts.tags ?? [])];
    const created = await zk.zotero.createItem(fieldsOf(card), {
      collections: opts.collection ? [opts.collection] : [],
      tags,
    });
    // The note is a second change, on the new item: withdrawing the item withdraws it too.
    if (opts.note) await zk.zotero.addNote(created.key, `<p>${escapeHtml(opts.note)}</p>`);

    s.seen.push(card.id);
    s.undo.push({ kind: "keep", card, itemKey: created.key, changes: [{ kind: "createItem", ...created }] });
    if (opts.collection) {
      s.recent = [opts.collection, ...s.recent.filter((k) => k !== opts.collection)].slice(0, LIMITS.recent);
    }
    return { itemKey: created.key, replaces: card.inLibrary };
  });
}

/** 1–9: keep into the n-th most recently used collection. */
export async function keepRecent(n: number) {
  const collection = (await load()).recent[n - 1];
  return collection ? keep({ collection }) : null;
}

/** The recent collections, for the numbers on the card (Reading / arXiv …). */
export async function recentCollections(): Promise<zk.Collection[]> {
  const [recent, all] = await Promise.all([load().then((s) => s.recent), zk.library.collections()]);
  return recent.flatMap((key) => all.filter((c) => c.key === key));
}

/**
 * Replaces an old item by the one just kept: copies the old item's notes onto the new item, then
 * asks the user to move the old item to Zotero's trash. If they say no, the copies are withdrawn
 * (they were never sent) and both items stay as they were. Its annotations stay with the old
 * item: they are pinned to its PDF. Resolves whether the old item was trashed.
 */
export async function replaceOld(oldKey: string, newKey: string): Promise<boolean> {
  const copies = [];
  for (const note of await zk.library.notes(oldKey)) copies.push(await zk.zotero.addNote(newKey, note.html));

  // The app asks the user ("Feed Riffle wants to move '…' to the Zotero trash"); the plugin
  // waits for the answer: the change if they said yes, null if not.
  const trashed = await zk.zotero.trash(oldKey);
  if (!trashed) {
    for (const copy of copies) await zk.zotero.cancel(copy.changeId);
    return false;
  }
  // Remember the trash with the keep, so undo puts the old item back.
  await withState((s) => {
    const last = s.undo.at(-1);
    if (last?.kind === "keep" && last.itemKey === newKey) last.changes.push({ kind: "trash", ...trashed });
  });
  return true;
}

/**
 * u Undo the last discard, skip or keep; the card comes back to the top of the deck. Undoing a
 * keep withdraws its changes if they haven't been synced yet (no confirmation: nothing left the
 * Kindle). Once synced, a new item can only be trashed — the user is asked — and an old item
 * already trashed in Zotero is restored there, by the user. Resolves a line for the screen.
 */
export async function undo(): Promise<string> {
  const step = await withState((s) => s.undo.pop());
  if (!step) return "Nothing to undo";

  const left: string[] = [];
  if (step.kind === "keep") {
    for (const change of step.changes.toReversed()) {
      if (await zk.zotero.cancel(change.changeId)) continue; // withdrawn, as if never asked
      if (change.kind === "createItem") {
        if (!(await zk.zotero.trash(change.key))) left.push("the new item stays in your library");
      } else {
        left.push("the old item is in Zotero's trash: restore it there");
      }
    }
  }

  await withState((s) => {
    s.deck = [step.card, ...s.deck.filter((c) => c.id !== step.card.id)];
    s.seen = s.seen.filter((id) => id !== step.card.id);
  });
  return [`Undid ${step.kind}`, ...left].join("; ");
}

// ───────────────────────────── The screen (placeholder) ─────────────────────────────

// Part 2 designs screens and what they are made of; until then a screen is registered but can't
// be opened. This shows what the deck screen will be given.
zk.screen("deck", async () => {
  const { deck } = await load();
  return { card: deck[0] ?? null, left: deck.length, recent: await recentCollections() };
});

// ───────────────────────────── Feed parsing ─────────────────────────────

// RSS 2.0, RSS 1.0 (RDF) and Atom, with txml — a small, dependency-free XML parser bundled into
// main.js by build.mjs. Feeds are read leniently: whatever field is there is used.

/** The parsed entries of a feed, as cards. (Exported for testing; the app ignores exports.) */
export function parseFeed(xml: string, feed: string): Card[] {
  // selfClosingTags: [] — txml treats <link> as an HTML void element by default, which would
  // lose RSS's <link>https://…</link>. decodeEntities turns &amp; etc. into characters
  // (CDATA sections are left as they are, as XML says).
  const nodes = parse(xml, { selfClosingTags: [], decodeEntities: true });
  const entries = filter(nodes, (n) => n.tagName === "item" || n.tagName === "entry");
  return entries.map((entry) => toCard(entry, feed)).filter((c) => c.title);
}

function toCard(entry: TNode, feed: string): Card {
  const link = atomLink(entry) ?? text(child(entry, "link"));
  const guid = text(child(entry, "guid") ?? child(entry, "id"));
  const hrefs = children(entry, "link").map((l) => l.attributes.href ?? "");
  const ids = [guid, link, text(child(entry, "dc:identifier")), ...hrefs].join(" ");
  const arxiv = ids.match(/arxiv\.org\/abs\/([^\s?#]+)|oai:arXiv\.org:(\S+)|arXiv:(\d{4}\.\d{4,5}(?:v\d+)?)/i)?.slice(1).find(Boolean) ?? null;
  const doi = (text(child(entry, "prism:doi") ?? child(entry, "arxiv:doi")) || ids.match(/\b10\.\d{4,9}\/\S+/)?.[0]) ?? null;
  const date = text(child(entry, "pubDate") ?? child(entry, "published") ?? child(entry, "dc:date") ?? child(entry, "updated"));
  return {
    id: arxiv ? `arxiv:${arxiv}` : doi ? `doi:${doi.toLowerCase()}` : guid || link || text(child(entry, "title")),
    feed,
    title: oneLine(stripTags(text(child(entry, "title")))),
    authors: authorsOf(entry),
    date: date ? isoDate(date) : null,
    link: link || null,
    summary: oneLine(stripTags(text(child(entry, "description") ?? child(entry, "summary") ?? child(entry, "content")))),
    doi: doi || null,
    arxiv,
    inLibrary: [],
  };
}

function child(node: TNode, name: string): TNode | undefined {
  return node.children.find((c): c is TNode => isElementNode(c) && c.tagName === name);
}

function children(node: TNode, name: string): TNode[] {
  return node.children.filter((c): c is TNode => isElementNode(c) && c.tagName === name);
}

/** All the text inside a node. */
function text(node: TNode | string | undefined): string {
  if (node === undefined) return "";
  if (typeof node === "string") return node;
  return node.children.map(text).join("").trim();
}

/** Atom's `<link rel="alternate" href="…"/>`. */
function atomLink(entry: TNode): string | null {
  const links = children(entry, "link").filter((l) => l.attributes.href);
  const page = links.find((l) => (l.attributes.rel ?? "alternate") === "alternate") ?? links[0];
  return page?.attributes.href ?? null;
}

/** Atom `<author><name>`, or `<dc:creator>` (arXiv lists them comma-separated in one), or RSS `<author>`. */
function authorsOf(entry: TNode): string[] {
  const atom = children(entry, "author").map((a) => text(child(a, "name"))).filter(Boolean);
  if (atom.length) return atom;
  const dc = children(entry, "dc:creator").map((c) => text(c));
  if (dc.length === 1) return dc[0]!.split(/,\s*(?:and\s+)?|\s+and\s+/).filter(Boolean);
  if (dc.length) return dc;
  return children(entry, "author").map((a) => text(a)).filter(Boolean);
}

/**
 * Markup out, crudely. (The desktop Feed Riffle renders feed HTML through a sanitizer and is
 * careful not to read `$1<p<\infty$` as a tag; part 2's screens will decide how much of that the
 * Kindle needs.)
 */
function stripTags(html: string): string {
  return html.replace(/<\/?[a-zA-Z][^>]*>/g, "");
}

function oneLine(s: string): string {
  return s.replace(/\s+/g, " ").trim();
}

/** `2024-01-03` from any date `Date` can read; the text as given otherwise. */
function isoDate(s: string): string {
  const d = new Date(s);
  return Number.isNaN(d.getTime()) ? s : d.toISOString().slice(0, 10);
}

// ───────────────────────────── Card → Zotero item ─────────────────────────────

function fieldsOf(card: Card): zk.ItemFields {
  return {
    type: card.arxiv ? "preprint" : "journalArticle",
    title: card.title,
    creators: card.authors.map(toCreator),
    date: card.date ?? undefined,
    doi: card.doi ?? undefined,
    arxiv: card.arxiv ?? undefined,
    url: card.link ?? undefined,
    abstract: card.summary || undefined,
  };
}

/** "Robert J. Adler" → first "Robert J.", last "Adler"; a one-word name stays whole. */
function toCreator(name: string): zk.Creator {
  const at = name.lastIndexOf(" ");
  return at < 0 ? { type: "author", name } : { type: "author", first: name.slice(0, at), last: name.slice(at + 1) };
}

function escapeHtml(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
