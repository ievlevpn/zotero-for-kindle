// Validates plugin.json files against types/plugin-manifest.schema.json.
// Usage: node validate-manifest.mjs <plugin.json>...
import Ajv from "ajv";
import { readFile } from "node:fs/promises";

const schema = JSON.parse(await readFile(new URL("types/plugin-manifest.schema.json", import.meta.url), "utf8"));
const validate = new Ajv({ allErrors: true }).compile(schema);
let failed = false;
for (const file of process.argv.slice(2)) {
  const ok = validate(JSON.parse(await readFile(file, "utf8")));
  if (ok) continue;
  failed = true;
  for (const e of validate.errors) console.error(`${file}: ${e.instancePath || "/"} ${e.message}`);
}
process.exit(failed ? 1 : 0);
