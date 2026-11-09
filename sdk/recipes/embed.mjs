// The recipes in docs/plugins/recipes.md are these files, embedded: a line
//
//   <!-- recipe: sdk/recipes/<name>.ts -->
//
// is followed by a ```ts block holding that file. check.sh runs this to fail when a block and
// its file differ (the files are type-checked; the Markdown can't drift from them).
//
//   node recipes/embed.mjs           # check
//   node recipes/embed.mjs --write   # copy the files into recipes.md
import { readdir, readFile, writeFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";

const root = fileURLToPath(new URL("../../", import.meta.url));
const docPath = resolve(root, "docs/plugins/recipes.md");
const write = process.argv.includes("--write");
const doc = await readFile(docPath, "utf8");

const marker = /<!-- recipe: (sdk\/recipes\/[\w.-]+\.ts) -->\n```ts\n([\s\S]*?)```\n/g;
let found = 0;
let drift = 0;
const out = [];
let last = 0;
for (const m of doc.matchAll(marker)) {
  found++;
  const source = await readFile(resolve(root, m[1]), "utf8");
  if (m[2] !== source) {
    drift++;
    if (!write) console.error(`docs/plugins/recipes.md: the block for ${m[1]} differs from the file`);
  }
  out.push(doc.slice(last, m.index), `<!-- recipe: ${m[1]} -->\n\`\`\`ts\n${source}\`\`\`\n`);
  last = m.index + m[0].length;
}
out.push(doc.slice(last));
const embedded = new Set([...doc.matchAll(marker)].map((m) => m[1]));
for (const name of await readdir(resolve(root, "sdk/recipes"))) {
  if (name.endsWith(".ts") && !embedded.has(`sdk/recipes/${name}`)) {
    console.error(`docs/plugins/recipes.md: sdk/recipes/${name} isn't in it (add <!-- recipe: sdk/recipes/${name} --> and a ts block)`);
    drift++;
  }
}
if (found === 0) {
  console.error("docs/plugins/recipes.md: no recipe blocks found");
  process.exit(1);
}
if (write) {
  await writeFile(docPath, out.join(""));
  console.log(`recipes.md: ${found} recipes written`);
} else if (drift) {
  console.error("  refresh: node sdk/recipes/embed.mjs --write");
  process.exit(1);
} else {
  console.log(`recipes.md: ${found} recipes match their files`);
}
