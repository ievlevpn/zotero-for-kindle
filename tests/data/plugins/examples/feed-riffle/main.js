// examples/feed-riffle/node_modules/txml/dist/txml.mjs
function e(e2) {
  return e2.replace(/&(#x[0-9a-fA-F]+|#\d+|amp|lt|gt|quot|apos);/g, function(e3, t) {
    if ("amp" === t) return "&";
    if ("lt" === t) return "<";
    if ("gt" === t) return ">";
    if ("quot" === t) return '"';
    if ("apos" === t) return "'";
    if ("#" === t[0]) {
      var r = "x" === t[1] || "X" === t[1], a2 = parseInt(t.slice(r ? 2 : 1), r ? 16 : 10);
      if (!Number.isNaN(a2) && a2 >= 0 && a2 <= 1114111) try {
        return String.fromCodePoint(a2);
      } catch (t2) {
        return e3;
      }
    }
    return e3;
  });
}
function a(t, r) {
  var a2 = (r = r || {}).pos || 0, i = !!r.keepComments, c = void 0 !== r.keepWhitespace ? !!r.keepWhitespace : !!r.keepWhitespaces, s = !!r.decodeEntities, f = !!r.skipXmlDeclaration, u = "<".charCodeAt(0), l = ">".charCodeAt(0), h2 = "-".charCodeAt(0), d = "/".charCodeAt(0), p = "?".charCodeAt(0), g = "!".charCodeAt(0), A = "'".charCodeAt(0), C = '"'.charCodeAt(0), m = "=".charCodeAt(0), v = "[".charCodeAt(0), y = "]".charCodeAt(0), b = "?".charCodeAt(0);
  function N(e2) {
    return "string" == typeof e2 && "?xml" === e2.toLowerCase();
  }
  function x(e2) {
    for (var r2 = []; t[a2]; ) if (t.charCodeAt(a2) == u) {
      if (t.charCodeAt(a2 + 1) === d) {
        var n2 = a2 + 2;
        if (a2 = t.indexOf(">", a2), -1 == t.substring(n2, a2).indexOf(e2)) {
          var o2 = t.substring(0, a2).split("\n");
          throw new Error("Unexpected close tag\nLine: " + (o2.length - 1) + "\nColumn: " + (o2[o2.length - 1].length + 1) + "\nChar: " + t[a2]);
        }
        return a2 + 1 && (a2 += 1), r2;
      }
      if (t.charCodeAt(a2 + 1) === g) {
        if (t.charCodeAt(a2 + 2) == h2) {
          const e3 = a2;
          for (; -1 !== a2 && (t.charCodeAt(a2) !== l || t.charCodeAt(a2 - 1) != h2 || t.charCodeAt(a2 - 2) != h2 || -1 == a2); ) a2 = t.indexOf(">", a2 + 1);
          -1 === a2 && (a2 = t.length), i && r2.push(t.substring(e3, a2 + 1));
        } else {
          if (t.charCodeAt(a2 + 2) === v && t.charCodeAt(a2 + 8) === v && "cdata" === t.substr(a2 + 3, 5).toLowerCase()) {
            var s2 = t.indexOf("]]>", a2);
            -1 == s2 ? (r2.push(t.substr(a2 + 9)), a2 = t.length) : (r2.push(t.substring(a2 + 9, s2)), a2 = s2 + 3);
            continue;
          }
          {
            const e3 = a2 + 1;
            a2 += 2;
            for (var p2 = false; (t.charCodeAt(a2) !== l || true === p2) && t[a2]; ) t.charCodeAt(a2) === v ? p2 = true : true === p2 && t.charCodeAt(a2) === y && (p2 = false), a2++;
            r2.push(t.substring(e3, a2));
          }
        }
        a2++;
        continue;
      }
      var A2 = W();
      if (f && N(A2.tagName)) continue;
      r2.push(A2);
    } else {
      var C2 = O();
      if (c) C2.length > 0 && r2.push(C2);
      else {
        var m2 = C2.trim();
        m2.length > 0 && r2.push(m2);
      }
      a2++;
    }
    return r2;
  }
  function O() {
    var r2 = a2;
    -2 === (a2 = t.indexOf("<", a2) - 1) && (a2 = t.length);
    var n2 = t.slice(r2, a2 + 1);
    return s ? e(n2) : n2;
  }
  var j = "\r\n	>/= ";
  function k() {
    for (var e2 = a2; -1 === j.indexOf(t[a2]) && t[a2]; ) a2++;
    return t.slice(e2, a2);
  }
  var E, w = r.selfClosingTags || r.noChildNodes || ["img", "br", "input", "meta", "link", "hr"];
  function W() {
    a2++;
    const r2 = k(), n2 = "?" === r2[0], i2 = a2, o2 = {};
    let c2 = [];
    for (; t[a2] && t.charCodeAt(a2) !== l && (!n2 || t.charCodeAt(a2) !== p || t.charCodeAt(a2 + 1) !== l); ) {
      var f2 = t.charCodeAt(a2);
      if (f2 > 64 && f2 < 91 || f2 > 96 && f2 < 123) {
        for (var u2 = k(), h3 = null; 32 === t.charCodeAt(a2) || 9 === t.charCodeAt(a2) || 10 === t.charCodeAt(a2) || 13 === t.charCodeAt(a2); ) a2++;
        if (t.charCodeAt(a2) === m) {
          for (a2++; 32 === t.charCodeAt(a2) || 9 === t.charCodeAt(a2) || 10 === t.charCodeAt(a2) || 13 === t.charCodeAt(a2); ) a2++;
          var g2 = t.charCodeAt(a2);
          if (g2 === A || g2 === C) {
            if (h3 = V(), -1 === a2) return { tagName: r2, attributes: o2, children: c2 };
          } else if (g2 && g2 !== l) {
            for (var v2 = a2; t[a2] && -1 === j.indexOf(t[a2]); ) a2++;
            h3 = t.slice(v2, a2), s && (h3 = e(h3));
          }
        }
        o2[u2] = h3;
      } else a2++;
    }
    const y2 = !n2 && t.charCodeAt(a2 - 1) === d;
    if (n2) {
      var N2 = t.slice(i2, a2).trim();
      return N2.length > 0 && 0 === Object.keys(o2).length && (c2 = [N2]), t.charCodeAt(a2) === p && t.charCodeAt(a2 + 1) === l ? a2 += 2 : t.charCodeAt(a2) === l && (a2 += 1), { tagName: r2, attributes: o2, children: c2 };
    }
    if (t.charCodeAt(a2 - 1) !== d && t.charCodeAt(a2 - 1) !== b) if ("script" == r2) {
      var O2 = a2 + 1;
      a2 = t.indexOf("</script>", a2), c2 = [t.slice(O2, a2)], a2 += 9;
    } else if ("style" == r2) {
      O2 = a2 + 1;
      a2 = t.indexOf("</style>", a2), c2 = [t.slice(O2, a2)], a2 += 8;
    } else -1 === w.indexOf(r2) ? (a2++, c2 = x(r2)) : a2++;
    else a2++;
    const E2 = { tagName: r2, attributes: o2, children: c2 };
    return y2 && Object.defineProperty(E2, "selfClosed", { value: true, enumerable: false, writable: true, configurable: true }), E2;
  }
  function V() {
    var r2 = t[a2], n2 = a2 + 1;
    a2 = t.indexOf(r2, n2);
    var i2 = t.slice(n2, a2);
    return s ? e(i2) : i2;
  }
  function L() {
    if (!r || !r.attrName || !r.attrValue) return -1;
    var e2 = new RegExp("\\s" + r.attrName + `\\s*=['"]` + r.attrValue + `['"]`).exec(t);
    return e2 ? e2.index : -1;
  }
  if (void 0 !== r.attrValue) for (r.attrName = r.attrName || "id", E = []; -1 !== (a2 = L()); ) -1 !== (a2 = t.lastIndexOf("<", a2)) && E.push(W()), t = t.substr(a2), a2 = 0;
  else if (r.parseNode) for (E = W(); f && "object" == typeof E && E && N(E.tagName); ) E = W();
  else E = x("");
  if (r.filter && Array.isArray(E) && (E = o(E, r.filter)), r.simplify) {
    return n(Array.isArray(E) ? E : [E]);
  }
  return r.setPos && "object" == typeof E && !Array.isArray(E) && (E.pos = a2), E;
}
function n(e2) {
  if ("string" == typeof e2) return n(a(e2));
  if (!Array.isArray(e2)) throw new TypeError("simplify() expects parsed node array or XML string");
  var t = {};
  if (!e2.length) return "";
  if (1 === e2.length && "string" == typeof e2[0]) return e2[0];
  for (var r in e2.forEach(function(e3) {
    if ("object" == typeof e3) {
      t[e3.tagName] || (t[e3.tagName] = []);
      var r2 = n(e3.children);
      t[e3.tagName].push(r2), Object.keys(e3.attributes).length && "object" == typeof r2 && !Array.isArray(r2) && (r2._attributes = e3.attributes);
    }
  }), t) 1 == t[r].length && (t[r] = t[r][0]);
  return t;
}
function o(e2, t, r = 0, a2 = "") {
  var n2 = [];
  return (function e3(r2, a3, i) {
    r2.forEach(function(r3, o2) {
      "object" == typeof r3 && (t(r3, o2, a3, i) && n2.push(r3), r3.children && e3(r3.children, a3 + 1, (i ? i + "." : "") + o2 + "." + r3.tagName));
    });
  })(e2, r, a2), n2;
}
function h(e2) {
  return "object" == typeof e2 && null !== e2 && "tagName" in e2;
}

// examples/feed-riffle/src/main.ts
var LIMITS = { seen: 5e3, undo: 50, recent: 6 };
var empty = () => ({ deck: [], seen: [], undo: [], recent: [], titles: {}, current: null, fresh: 0 });
async function load() {
  return { ...empty(), ...await zk.storage.get("state") };
}
async function withState(change) {
  let result;
  await zk.storage.update("state", async (old) => {
    const state = { ...empty(), ...old };
    result = await change(state);
    state.seen = state.seen.slice(-LIMITS.seen);
    state.undo = state.undo.slice(-LIMITS.undo);
    return state;
  });
  return result;
}
function cardsIn(s) {
  return s.current ? s.deck.filter((c) => c.feed === s.current) : s.deck;
}
function takeCard(s, id) {
  const at = id ? s.deck.findIndex((c) => c.id === id) : s.deck.findIndex((c) => !s.current || c.feed === s.current);
  return at < 0 ? void 0 : s.deck.splice(at, 1)[0];
}
function feedName(s, feed) {
  if (!feed) return "All feeds";
  return s.titles[feed] ?? (URL.canParse(feed) ? new URL(feed).hostname : feed);
}
zk.background(async () => {
  const added = await refreshFeeds();
  if (added > 0) {
    const fresh = await withState((s) => s.fresh += added);
    zk.setBadge(`${fresh} new`);
  }
  zk.ui.refresh();
});
async function refreshFeeds() {
  const before = await load();
  const known = /* @__PURE__ */ new Set([...before.seen, ...before.deck.map((c) => c.id)]);
  const fresh = [];
  const titles = {};
  for (const url of zk.settings.feeds ?? []) {
    try {
      const res = await zk.fetch(url, { timeout: 8e3 });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const feed = parseFeed(await res.text(), url);
      if (feed.title) titles[url] = feed.title;
      for (const card of feed.cards) {
        if (known.has(card.id)) continue;
        known.add(card.id);
        card.inLibrary = await inLibrary(card);
        fresh.push(card);
      }
    } catch (e2) {
      console.warn("skipped feed", url, String(e2));
    }
  }
  const added = await withState((s) => {
    const dealt = /* @__PURE__ */ new Set([...s.seen, ...s.deck.map((c) => c.id)]);
    const add = fresh.filter((c) => !dealt.has(c.id));
    s.deck.push(...add);
    Object.assign(s.titles, titles);
    return add.length;
  });
  console.log(`${added} new cards`);
  return added;
}
async function inLibrary(card) {
  if (!card.doi && !card.arxiv) return [];
  const items = await zk.library.findByIdentifier({ doi: card.doi ?? void 0, arxiv: card.arxiv ?? void 0 });
  return items.map((i) => i.key);
}
var sitting = { kept: 0, discarded: 0, skipped: 0, ms: 0, last: 0, dealt: 0 };
function tally(kind, by = 1) {
  const key = kind === "keep" ? "kept" : kind === "discard" ? "discarded" : "skipped";
  sitting[key] = Math.max(0, sitting[key] + by);
  const now = Date.now();
  if (by > 0 && sitting.last) sitting.ms += Math.min(now - sitting.last, 5 * 6e4);
  sitting.last = now;
}
async function discard() {
  const card = await withState((s) => {
    const card2 = takeCard(s);
    if (card2) {
      s.seen.push(card2.id);
      s.undo.push({ kind: "discard", card: card2 });
    }
    return card2;
  });
  if (card) tally("discard");
  return card;
}
async function skip() {
  const card = await withState((s) => {
    const card2 = takeCard(s);
    if (card2) {
      s.deck.push(card2);
      s.undo.push({ kind: "skip", card: card2 });
    }
    return card2;
  });
  if (card) tally("skip");
  return card;
}
async function keep(opts = {}) {
  const kept = await withState(async (s) => {
    const card = takeCard(s, opts.cardId);
    if (!card) return null;
    const tags = [.../* @__PURE__ */ new Set([...zk.settings.tags ?? [], ...opts.tags ?? []])];
    const created = await zk.zotero.createItem(fieldsOf(card), {
      collections: opts.collection ? [opts.collection] : [],
      tags
    });
    if (opts.note?.trim()) await zk.zotero.addNote(created.key, `<p>${escapeHtml(opts.note.trim())}</p>`);
    s.seen.push(card.id);
    s.undo.push({ kind: "keep", card, itemKey: created.key, changes: [{ kind: "createItem", ...created }] });
    if (opts.collection) {
      s.recent = [opts.collection, ...s.recent.filter((k) => k !== opts.collection)].slice(0, LIMITS.recent);
    }
    return { itemKey: created.key, replaces: card.inLibrary };
  });
  if (kept) tally("keep");
  return kept;
}
async function replaceOld(oldKey, newKey) {
  const copies = [];
  for (const note of await zk.library.notes(oldKey)) copies.push(await zk.zotero.addNote(newKey, note.html));
  const trashed = await zk.zotero.trash(oldKey);
  if (!trashed) {
    for (const copy of copies) await zk.zotero.cancel(copy.changeId);
    return false;
  }
  await withState((s) => {
    const last = s.undo.at(-1);
    if (last?.kind === "keep" && last.itemKey === newKey) last.changes.push({ kind: "trash", ...trashed });
  });
  return true;
}
async function undo() {
  const step = await withState((s) => s.undo.pop());
  if (!step) return "Nothing to undo";
  const left = [];
  if (step.kind === "keep") {
    for (const change of step.changes.toReversed()) {
      if (await zk.zotero.cancel(change.changeId)) continue;
      if (change.kind === "createItem") {
        if (!await zk.zotero.trash(change.key)) left.push("the new item stays in your library");
      } else {
        left.push("the old item is in Zotero's trash: restore it there");
      }
    }
  }
  await withState((s) => {
    s.deck = [step.card, ...s.deck.filter((c) => c.id !== step.card.id)];
    s.seen = s.seen.filter((id) => id !== step.card.id);
  });
  tally(step.kind, -1);
  const done = { discard: "Undid the discard", skip: "Undid the skip", keep: "Undid the keep" }[step.kind];
  return [done, ...left].join("; ");
}
var message = null;
zk.screen("deck", {
  title: "Feed Riffle",
  main: true,
  render: async () => {
    const s = await load();
    const cards = cardsIn(s);
    const top = cards[0];
    const header = zk.ui.row([
      zk.ui.text(`${feedName(s, s.current)} \xB7 ${cards.length} left`, { style: "label" }),
      zk.ui.button({ id: "feeds", title: "Feeds" })
    ]);
    if (!top) {
      return zk.ui.column([
        header,
        ...summary(s),
        zk.ui.buttons([{ id: "undo", title: "Undo" }]),
        message && zk.ui.notice(message)
      ]);
    }
    const recent = await recentCollections(s.recent);
    return zk.ui.column([
      header,
      // Swipe left: discard; right: keep. The app turns the card like a page.
      zk.ui.deck({ id: "card", card: cardBlock(top, !s.current ? feedName(s, top.feed) : null) }),
      // Recent collections: one tap files the card there (the desktop's keys 1–9).
      recent.length > 0 && zk.ui.text("File into", { style: "label" }),
      recent.length > 0 && zk.ui.chips({ id: "recent", select: "none", chips: recent.map((c) => ({ id: c.key, title: c.name })) }),
      zk.ui.buttons([
        { id: "discard", title: "Discard" },
        { id: "skip", title: "Skip" },
        { id: "keep", title: "Keep", primary: true }
      ]),
      zk.ui.buttons([
        { id: "undo", title: "Undo" },
        { id: "done", title: "Done" }
      ]),
      message && zk.ui.notice(message)
    ]);
  },
  onEvent: async (e2) => {
    switch (e2.kind) {
      case "open": {
        const s = await withState((s2) => {
          s2.fresh = 0;
          return s2;
        });
        sitting = { kept: 0, discarded: 0, skipped: 0, ms: 0, last: 0, dealt: cardsIn(s).length };
        message = null;
        zk.setBadge(null);
        return;
      }
      case "swipe":
        return e2.direction === "left" ? onDiscard() : onKeep();
      case "tap":
        if (e2.id === "discard") return onDiscard();
        if (e2.id === "keep") return onKeep();
        if (e2.id === "skip") {
          const card = await skip();
          message = card ? `Skipped \u201C${short(card.title)}\u201D` : null;
        }
        if (e2.id === "undo") message = await undo();
        if (e2.id === "feeds") zk.ui.open("feeds");
        if (e2.id === "done") zk.ui.open("summary");
        return;
      case "chips": {
        const kept = await keep({ collection: e2.chip });
        const name = (await zk.library.collections()).find((c) => c.key === e2.chip)?.name ?? "the collection";
        message = kept ? `Kept into ${name}` : null;
        return;
      }
      case "select":
        message = await switchFeed(e2.row);
        return;
      case "result":
        if (typeof e2.value === "string") message = e2.value;
        return;
    }
  }
});
async function onDiscard() {
  const card = await discard();
  message = card ? `Discarded \u201C${short(card.title)}\u201D` : null;
}
async function onKeep() {
  const top = cardsIn(await load())[0];
  if (top) zk.ui.open("file", { cardId: top.id });
}
function cardBlock(card, feed) {
  const authors = card.authors.length > 3 ? `${card.authors.slice(0, 3).join(", ")} et al.` : card.authors.join(", ");
  return zk.ui.card({
    title: card.title,
    meta: [authors, card.date, feed].filter(Boolean).join(" \xB7 "),
    body: card.summary,
    // plain text; LaTeX stays as source
    badge: card.inLibrary.length ? "In library" : void 0
  });
}
async function recentCollections(recent) {
  if (!recent.length) return [];
  const all = await zk.library.collections();
  return recent.flatMap((key) => all.filter((c) => c.key === key));
}
function summary(s) {
  const { kept, discarded, skipped, ms, dealt } = sitting;
  const decided = kept + discarded;
  const left = cardsIn(s).length;
  const minutes = Math.round(ms / 6e4);
  const perCard = decided + skipped ? Math.round(ms / 1e3 / (decided + skipped)) : 0;
  const others = feedCounts(s).filter((f) => f.count > 0 && f.feed !== s.current);
  return [
    zk.ui.text(dealt ? `${decided} of ${dealt} cleared` : "Nothing to riffle", { style: "title" }),
    zk.ui.text(
      dealt ? `${skipped} skipped for later, ${left} still to riffle` : "Feeds are fetched after a sync, at most hourly \u2014 or Feeds \u2192 Fetch now.",
      { style: "meta" }
    ),
    decided + skipped > 0 && zk.ui.text(`${kept} kept \xB7 ${discarded} discarded \xB7 ${skipped} skipped \xB7 ${minutes} min \xB7 ${perCard} s a card`, { style: "meta" }),
    others.length > 0 && zk.ui.divider(),
    others.length > 0 && zk.ui.text("Still to riffle", { style: "label" }),
    others.length > 0 && zk.ui.list({ id: "next", rows: others.map((f) => ({ id: f.feed, title: feedName(s, f.feed), meta: `${f.count}` })) })
  ];
}
zk.screen("summary", {
  title: "The sitting",
  render: async () => zk.ui.column(summary(await load())),
  onEvent: async (e2) => {
    if (e2.kind === "select") zk.ui.close(await switchFeed(e2.row));
  }
});
function feedCounts(s) {
  const feeds = [.../* @__PURE__ */ new Set([...zk.settings.feeds ?? [], ...s.deck.map((c) => c.feed)])];
  return feeds.map((feed) => ({ feed, count: s.deck.filter((c) => c.feed === feed).length }));
}
async function switchFeed(feed) {
  const s = await withState((s2) => {
    s2.current = feed === "*" ? null : feed;
    return s2;
  });
  return `Riffling ${feedName(s, s.current)}`;
}
var fetched = null;
zk.screen("feeds", {
  title: "Feeds",
  render: async () => {
    const s = await load();
    const rows = [
      { id: "*", title: "All feeds", meta: `${s.deck.length}`, selected: s.current === null },
      ...feedCounts(s).map(({ feed, count }) => ({
        id: feed,
        title: feedName(s, feed),
        subtitle: feed,
        meta: `${count}`,
        selected: s.current === feed
      }))
    ];
    return zk.ui.column([
      zk.ui.list({ id: "feeds", rows }),
      zk.ui.button({ id: "fetch", title: "Fetch now" }),
      fetched && zk.ui.notice(fetched)
    ]);
  },
  onEvent: async (e2) => {
    if (e2.kind === "open") fetched = null;
    if (e2.kind === "select") zk.ui.close(await switchFeed(e2.row));
    if (e2.kind === "tap" && e2.id === "fetch") {
      const added = await refreshFeeds();
      fetched = added ? `${added} new card${added === 1 ? "" : "s"}` : "Nothing new";
    }
  }
});
var draft = null;
zk.screen("file", {
  title: "Keep",
  render: () => {
    const d = draft;
    if (!d) return zk.ui.notice("This card is no longer in the deck.");
    const chosen = d.collections.find((c) => c.key === d.collection);
    const notes = d.old.reduce((n2, o2) => n2 + o2.notes, 0);
    const suggestions = d.tagText ? d.libraryTags.filter((t) => !d.tags.includes(t) && t.toLowerCase().includes(d.tagText.toLowerCase())).slice(0, 8) : [];
    return zk.ui.column([
      zk.ui.text(d.card.title, { style: "title", lines: 2 }),
      d.old.length > 0 && zk.ui.notice(`Already in your library \u2014 filing adds a second item. The old one holds ${notes} note${notes === 1 ? "" : "s"}.`),
      // A one-chip set reads as a switch: on, the old copy goes to the trash (after the app asks).
      d.old.length > 0 && zk.ui.chips({
        id: "replace",
        chips: [{ id: "trash", title: "Move the old one to the trash" }],
        selected: d.replace ? ["trash"] : []
      }),
      zk.ui.search({ id: "find", placeholder: "Collection", focus: true }),
      zk.ui.list({ id: "collections", rows: collectionRows(d), empty: "No collection matches" }),
      zk.ui.text("Tags", { style: "label" }),
      // Chosen tags are selected chips (tap to drop); matches for what is typed follow, unselected.
      (d.tags.length > 0 || suggestions.length > 0) && zk.ui.chips({ id: "tags", chips: [...d.tags, ...suggestions], selected: d.tags }),
      zk.ui.field({ id: "tag", placeholder: "Add a tag", value: d.tagText }),
      zk.ui.field({ id: "note", label: "Note", lines: 3 }),
      zk.ui.button({ id: "file", title: chosen ? `File into ${chosen.name}` : "Keep (no collection)", primary: true })
    ]);
  },
  onEvent: async (e2, { cardId }) => {
    if (e2.kind === "open") {
      draft = await startDraft(cardId);
      return;
    }
    const d = draft;
    if (!d) return;
    switch (e2.kind) {
      case "change":
        if (e2.id === "find") d.query = e2.text;
        if (e2.id === "tag") d.tagText = e2.text;
        if (e2.id === "note") d.note = e2.text;
        return;
      case "submit":
        if (e2.id === "find") d.collection = matches(d)[0]?.key ?? d.collection;
        if (e2.id === "tag" && e2.text.trim()) {
          d.tags = [.../* @__PURE__ */ new Set([...d.tags, e2.text.trim()])];
          d.tagText = "";
        }
        return;
      case "select":
        d.collection = e2.row;
        return;
      case "chips":
        if (e2.id === "replace") d.replace = e2.selected.includes("trash");
        if (e2.id === "tags") {
          d.tags = e2.selected;
          d.tagText = "";
        }
        return;
      case "tap":
        if (e2.id === "file") zk.ui.close(await fileDraft(d));
        return;
      case "back":
        draft = null;
        return;
    }
  }
});
async function startDraft(cardId) {
  const s = await load();
  const card = s.deck.find((c) => c.id === cardId);
  if (!card) return null;
  const [collections, tags] = await Promise.all([zk.library.collections(), zk.library.tags()]);
  const old = [];
  for (const key of card.inLibrary) {
    const item = await zk.library.item(key);
    if (item) old.push({ key, title: item.title, notes: (await zk.library.notes(key)).length, collections: item.collections });
  }
  return {
    card,
    collections,
    libraryTags: tags.map((t) => t.name),
    old,
    query: "",
    // Pointed at the collection used last, so File alone files it there.
    collection: s.recent[0] ?? null,
    tags: [],
    tagText: "",
    note: "",
    replace: false
  };
}
function matches(d) {
  const words = d.query.toLowerCase().split(/\s+/).filter(Boolean);
  const hoisted = new Set(d.old.flatMap((o2) => o2.collections));
  const found = d.collections.filter((c) => words.every((w) => c.path.join(" / ").toLowerCase().includes(w)));
  return found.sort((a2, b) => Number(hoisted.has(b.key)) - Number(hoisted.has(a2.key)));
}
function collectionRows(d) {
  const hoisted = new Set(d.old.flatMap((o2) => o2.collections));
  return matches(d).slice(0, 200).map((c) => ({
    id: c.key,
    title: c.path.join(" / "),
    badge: hoisted.has(c.key) ? "already in" : void 0,
    selected: c.key === d.collection
  }));
}
async function fileDraft(d) {
  draft = null;
  const kept = await keep({ cardId: d.card.id, collection: d.collection, tags: d.tags, note: d.note });
  if (!kept) return "That card is no longer in the deck";
  const where = d.collections.find((c) => c.key === d.collection)?.name;
  let line = where ? `Kept into ${where}` : "Kept";
  if (d.replace && kept.replaces.length) {
    let trashed = 0;
    for (const oldKey of kept.replaces) if (await replaceOld(oldKey, kept.itemKey)) trashed++;
    line += trashed ? "; the old one is in the trash (Undo puts it back)" : "; kept the old one too";
  }
  return line;
}
function short(title) {
  return title.length > 60 ? `${title.slice(0, 57)}\u2026` : title;
}
function parseFeed(xml, feed) {
  const nodes = a(xml, { selfClosingTags: [], decodeEntities: true });
  const entries = o(nodes, (n2) => n2.tagName === "item" || n2.tagName === "entry");
  const channel = o(nodes, (n2) => n2.tagName === "channel" || n2.tagName === "feed")[0];
  const title = channel ? oneLine(stripTags(text(child(channel, "title")))) : "";
  return { title: title || null, cards: entries.map((entry) => toCard(entry, feed)).filter((c) => c.title) };
}
function toCard(entry, feed) {
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
    inLibrary: []
  };
}
function child(node, name) {
  return node.children.find((c) => h(c) && c.tagName === name);
}
function children(node, name) {
  return node.children.filter((c) => h(c) && c.tagName === name);
}
function text(node) {
  if (node === void 0) return "";
  if (typeof node === "string") return node;
  return node.children.map(text).join("").trim();
}
function atomLink(entry) {
  const links = children(entry, "link").filter((l) => l.attributes.href);
  const page = links.find((l) => (l.attributes.rel ?? "alternate") === "alternate") ?? links[0];
  return page?.attributes.href ?? null;
}
function authorsOf(entry) {
  const atom = children(entry, "author").map((a2) => text(child(a2, "name"))).filter(Boolean);
  if (atom.length) return atom;
  const dc = children(entry, "dc:creator").map((c) => text(c));
  if (dc.length === 1) return dc[0].split(/,\s*(?:and\s+)?|\s+and\s+/).filter(Boolean);
  if (dc.length) return dc;
  return children(entry, "author").map((a2) => text(a2)).filter(Boolean);
}
function stripTags(html) {
  return html.replace(/<\/?[a-zA-Z][^>]*>/g, "");
}
function oneLine(s) {
  return s.replace(/\s+/g, " ").trim();
}
function isoDate(s) {
  const d = new Date(s);
  return Number.isNaN(d.getTime()) ? s : d.toISOString().slice(0, 10);
}
function fieldsOf(card) {
  return {
    type: card.arxiv ? "preprint" : "journalArticle",
    title: card.title,
    creators: card.authors.map(toCreator),
    date: card.date ?? void 0,
    doi: card.doi ?? void 0,
    arxiv: card.arxiv ?? void 0,
    url: card.link ?? void 0,
    abstract: card.summary || void 0
  };
}
function toCreator(name) {
  const at = name.lastIndexOf(" ");
  return at < 0 ? { type: "author", name } : { type: "author", first: name.slice(0, at), last: name.slice(at + 1) };
}
function escapeHtml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
export {
  parseFeed
};
