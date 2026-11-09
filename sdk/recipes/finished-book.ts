// When you turn past the last page of a book, tag it and add a note with the date.
// plugin.json: "permissions": ["library:read", "zotero:tags", "zotero:notes"]

zk.on("bookFinished", async ({ item }) => {
  await zk.zotero.addTag(item.key, "finished-on-kindle");
  const day = new Date().toISOString().slice(0, 10);
  await zk.zotero.addNote(item.key, `<p>Finished on the Kindle on ${day}.</p>`);
  console.log(`finished "${item.title}"`);
});
