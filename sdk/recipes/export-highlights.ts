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
