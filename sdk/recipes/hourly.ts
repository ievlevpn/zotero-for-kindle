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
