// A form: a field, chips and a button on a screen that an item's Plugins section opens. The
// button writes to Zotero and closes the screen.
// plugin.json: "permissions": ["zotero:tags"]

let tags: string[] = [];
let text = "";

zk.screen<{ itemKey: string }>("tag-it", {
  title: "Tag",
  render: () =>
    zk.ui.column([
      zk.ui.field({ id: "tag", placeholder: "New tag", value: text }),
      zk.ui.chips({ id: "picked", chips: ["to-read", "important", ...tags.filter((t) => t !== "to-read" && t !== "important")], selected: tags }),
      zk.ui.button({ id: "save", title: "Add tags", primary: true }),
    ]),
  onEvent: async (e, { itemKey }) => {
    if (e.kind === "change") text = e.text;
    else if (e.kind === "submit" && e.text.trim()) {
      tags = [...new Set([...tags, e.text.trim()])];
      text = ""; // differs from what was typed, so the app clears the box
    } else if (e.kind === "chips") tags = e.selected;
    else if (e.kind === "tap" && e.id === "save") {
      for (const t of tags) await zk.zotero.addTag(itemKey, t);
      const n = tags.length;
      tags = [];
      zk.ui.close(`Added ${n} tag${n === 1 ? "" : "s"}`);
    }
  },
});

zk.itemAction({
  id: "tag-it",
  title: "Tag…",
  run: (item) => {
    zk.ui.open("tag-it", { itemKey: item.key });
  },
});
