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
