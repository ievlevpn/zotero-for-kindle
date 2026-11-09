// examples/highlights-to-markdown/src/main.ts
var empty = () => ({ books: {}, todo: [] });
async function loadState() {
  return await zk.storage.get("state") ?? empty();
}
async function updateState(change) {
  await zk.storage.update("state", async (old) => {
    const state = old ?? empty();
    await change(state);
    return state;
  });
}
async function exportBook(itemKey, state) {
  const previous = state.books[itemKey];
  const item = await zk.library.item(itemKey);
  const parts = [];
  if (item) {
    for (const attachment of await zk.library.attachments(item.key)) {
      const annotations = (await zk.library.annotations(attachment.key)).filter((a) => a.text || a.comment);
      if (annotations.length > 0) parts.push({ attachment, annotations });
    }
  }
  const highlights = parts.reduce((n, p) => n + p.annotations.length, 0);
  if (!item || highlights === 0) {
    if (previous) {
      await zk.files.remove(previous.file);
      delete state.books[itemKey];
      console.log("removed", previous.file);
    }
    return;
  }
  const file = fileNameFor(item, state);
  await zk.files.write(file, toMarkdown(item, parts));
  if (previous && previous.file !== file) await zk.files.remove(previous.file);
  state.books[itemKey] = { file, highlights, written: (/* @__PURE__ */ new Date()).toISOString() };
  console.log(`wrote ${file} (${highlights} highlights)`);
}
async function exportBooks(itemKeys) {
  for (const key of new Set(itemKeys)) {
    await updateState(async (state) => {
      await exportBook(key, state);
      state.todo = state.todo.filter((k) => k !== key);
    });
  }
}
async function addTodo(itemKeys) {
  await updateState((state) => {
    state.todo = [.../* @__PURE__ */ new Set([...state.todo, ...itemKeys])];
  });
}
async function onAnnotations(origin, annotations) {
  const books = annotations.map((a) => a.itemKey);
  await addTodo(books);
  if (origin === "kindle") await exportBooks(books);
}
zk.on("highlightCreated", ({ origin, annotations }) => onAnnotations(origin, annotations));
zk.on("highlightChanged", ({ origin, annotations }) => onAnnotations(origin, annotations));
zk.on("highlightDeleted", ({ origin, annotations }) => onAnnotations(origin, annotations));
var WRITE_AFTER_SYNC = 3;
zk.on("syncFinished", async ({ initial }) => {
  if (initial) return;
  await exportBooks((await loadState()).todo.slice(0, WRITE_AFTER_SYNC));
});
zk.background(async () => {
  const started = (/* @__PURE__ */ new Date()).toISOString();
  const { lastRun } = await loadState();
  const changed = /* @__PURE__ */ new Set();
  for await (const a of zk.library.recentAnnotations({ since: lastRun })) changed.add(a.itemKey);
  await updateState((state) => {
    state.todo = [.../* @__PURE__ */ new Set([...state.todo, ...changed])];
    state.lastRun = started;
  });
  await exportBooks((await loadState()).todo);
});
function fileNameFor(item, state) {
  const base = (item.citationKey ?? item.title).replace(/[\\/:*?"<>|\u0000-\u001f]+/g, " ").replace(/\s+/g, " ").trim().slice(0, 100) || item.key;
  const taken = Object.entries(state.books).some(([key, b]) => key !== item.key && b.file === `${base}.md`);
  return taken ? `${base} (${item.key}).md` : `${base}.md`;
}
function authors(item) {
  const names = item.creators.filter((c) => c.type === "author" || c.type === "editor").map((c) => c.name ?? [c.first, c.last].filter(Boolean).join(" "));
  return names.length > 3 ? `${names[0]} et al.` : names.join(", ");
}
function toMarkdown(item, parts) {
  const byline = [
    [authors(item), item.year ? `(${item.year})` : ""].filter(Boolean).join(" "),
    item.publication ? `*${item.publication}*` : "",
    item.citationKey ? `Citation key: \`${item.citationKey}\`` : ""
  ].filter(Boolean);
  const lines = [`# ${item.title || item.key}`, ""];
  if (byline.length) lines.push(`${byline.join(". ")}`, "");
  for (const { attachment, annotations } of parts) {
    if (parts.length > 1) lines.push(`## ${attachment.title}`, "");
    for (const a of annotations) {
      if (a.text) lines.push(...a.text.trim().split("\n").map((line) => `> ${line}`), "");
      const where = a.pageLabel ? `\u2014 p. ${a.pageLabel}` : "";
      const comment = a.comment?.trim() ?? "";
      if (comment || where) lines.push([comment, where].filter(Boolean).join(" "), "");
    }
  }
  return lines.join("\n");
}
