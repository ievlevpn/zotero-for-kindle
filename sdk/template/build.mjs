// Builds the plugin: bundles src/main.ts and what it imports into one ES module,
// dist/<id>/main.js, and copies plugin.json next to it. dist/<id>/ is the plugin folder
// to copy to /mnt/us/zotero/plugins/ on the Kindle.
import { build } from "esbuild";
import { copyFile, mkdir, readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";

const here = (path) => fileURLToPath(new URL(path, import.meta.url));
const manifest = JSON.parse(await readFile(here("plugin.json"), "utf8"));
const out = here(`dist/${manifest.id}/`);

await mkdir(out, { recursive: true });
await build({
  entryPoints: [here("src/main.ts")],
  outfile: `${out}${manifest.main ?? "main.js"}`,
  bundle: true,
  format: "esm",
  target: "es2023",
  platform: "neutral",
  logLevel: "warning",
});
await copyFile(here("plugin.json"), `${out}plugin.json`);
console.log(`built ${out}`);
