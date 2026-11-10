// A list with search: type to narrow the collections, tap a row to see where it is.
// plugin.json: "permissions": ["library:read"]

let query = "";
let picked: string | null = null;

zk.screen("collections", {
  title: "Collections",
  main: true,
  render: async () => {
    const words = query.toLowerCase().split(/\s+/).filter(Boolean);
    const all = await zk.library.collections();
    const rows = all
      .filter((c) => words.every((w) => c.path.join(" / ").toLowerCase().includes(w)))
      .map((c) => ({ id: c.key, title: c.name, subtitle: c.path.join(" / "), selected: c.key === picked }));
    return zk.ui.column([
      zk.ui.search({ id: "find", placeholder: "Collection" }),
      zk.ui.list({ id: "rows", rows, empty: "No collection matches" }),
      picked && zk.ui.notice(`Picked ${picked}`),
    ]);
  },
  onEvent: (e) => {
    if (e.kind === "open") {
      query = "";
      picked = null;
    } else if (e.kind === "change") query = e.text;
    else if (e.kind === "select") picked = e.row;
  },
});
