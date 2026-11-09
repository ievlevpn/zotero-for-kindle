// A Zotero for Kindle plugin, in one file.
//
// The app runs this module once, when the plugin is first needed, on its own plugin thread.
// Its job at that moment is to say what it reacts to: register event handlers (zk.on), a
// background task (zk.background) or actions. There is one global, `zk`; its types, with a
// doc comment and an example for every member, are in sdk/types/zk.d.ts.
//
// `npm run build` bundles this file (and anything it imports) into dist/hello/main.js, next to
// a copy of plugin.json. Run the desktop build with --plugins-dir dist, or copy plugin.json and
// main.js from dist/hello/ to /mnt/us/zotero/plugins/hello/ on the Kindle (only those two: the
// desktop run keeps its log and data next to them).

// Code at the top level runs once, when the plugin loads. console.log writes to the plugin's
// log: plugin.log in its folder, and the terminal of the desktop build.
console.log(`${zk.plugin.id} ${zk.plugin.version} loaded`);

// bookFinished fires when you turn past the last page of a book or paper on the Kindle. The
// handler gets the item (title, creators, year…) and the file you read. Because it carries the
// item, listening to it needs "library:read" in plugin.json; without it, this line would throw a
// PermissionError saying exactly that.
zk.on("bookFinished", ({ item, attachment }) => {
  const authors = item.creators.map((c) => c.last ?? c.name).join(", ");
  console.log(`Finished "${item.title}" by ${authors || "unknown"} (${attachment.contentType})`);
});
