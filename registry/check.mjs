#!/usr/bin/env node
// Validates registry.json with the rules the app applies (src/catalog/registry.cpp): the whole
// file is refused for one bad part.
//
//   node registry/check.mjs registry/registry.json
//   node registry/check.mjs registry/registry.json --online   also: each repo's latest release
//                                                              has a plugin.zip whose signature verifies
import { verifyZip } from "../sdk/zk-plugin.mjs";
import { readFileSync } from "node:fs";

const ID = /^[a-z0-9]+(-[a-z0-9]+)*$/;                                   // manifest.cpp idRe, at most 64
const REPO = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})\/[A-Za-z0-9._-]{1,100}$/;  // registry.cpp validRepo
const MAX_BYTES = 1 << 20;

// Returns a list of problems (empty when the file is good).
export function checkRegistry(text) {
  const bad = [];
  if (Buffer.byteLength(text) > MAX_BYTES) return ["over 1 MB"];
  let j;
  try { j = JSON.parse(text); } catch { return ["not JSON"]; }
  if (j === null || typeof j !== "object" || Array.isArray(j)) return ["not an object"];
  if (j.registry !== 1) bad.push('"registry" must be 1');
  if (j.updated !== undefined && typeof j.updated !== "string") bad.push('"updated" must be a string');
  if (!Array.isArray(j.plugins)) return [...bad, '"plugins" must be an array'];
  const seen = new Set();
  const str = (o, k, where, required) => {
    if (o[k] === undefined) { if (required) bad.push(`${where}: missing "${k}"`); return undefined; }
    if (typeof o[k] !== "string") { bad.push(`${where}: "${k}" must be a string`); return undefined; }
    return o[k];
  };
  for (const [i, p] of j.plugins.entries()) {
    if (p === null || typeof p !== "object" || Array.isArray(p)) { bad.push(`plugins[${i}] is not an object`); continue; }
    const id = str(p, "id", `plugins[${i}]`, true);
    const where = `plugin ${id ?? i}`;
    if (id !== undefined) {
      if (id.length > 64 || !ID.test(id)) bad.push(`${where}: bad id`);
      if (seen.has(id)) bad.push(`${where}: duplicate id`);
      seen.add(id);
    }
    str(p, "name", where, true);
    str(p, "author", where, false);
    str(p, "description", where, false);
    const repo = str(p, "repo", where, true);
    if (repo !== undefined && (repo.length > 140 || !REPO.test(repo))) bad.push(`${where}: "repo" must be owner/name`);
    const key = str(p, "publicKey", where, true);
    if (key !== undefined && (!/^[A-Za-z0-9+/]+={0,2}$/.test(key) || Buffer.from(key, "base64").length !== 32))
      bad.push(`${where}: "publicKey" must be 32 bytes in base64`);
  }
  if (j.revoked !== undefined) {
    if (!Array.isArray(j.revoked)) bad.push('"revoked" must be an array');
    else for (const [i, v] of j.revoked.entries()) {
      if (v === null || typeof v !== "object" || Array.isArray(v)) { bad.push(`revoked[${i}] is not an object`); continue; }
      const id = str(v, "id", `revoked[${i}]`, true);
      if (id !== undefined && (id.length > 64 || !ID.test(id))) bad.push(`revoked[${i}]: bad id`);
      str(v, "reason", `revoked[${i}]`, true);
    }
  }
  return bad;
}

async function online(plugins) {
  const bad = [];
  const get = (url, accept) => fetch(url, { headers: { Accept: accept, "User-Agent": "zk-registry-check" }, redirect: "follow" });
  for (const p of plugins) {
    try {
      const rel = await get(`https://api.github.com/repos/${p.repo}/releases/latest`, "application/vnd.github+json");
      if (!rel.ok) { bad.push(`${p.id}: ${p.repo} has no latest release (HTTP ${rel.status})`); continue; }
      const assets = (await rel.json()).assets ?? [];
      const url = (n) => assets.find((a) => a.name === n)?.browser_download_url;
      if (!url("plugin.zip") || !url("plugin.zip.sig")) { bad.push(`${p.id}: the latest release lacks plugin.zip or plugin.zip.sig`); continue; }
      const zip = Buffer.from(await (await get(url("plugin.zip"), "application/octet-stream")).arrayBuffer());
      const sig = await (await get(url("plugin.zip.sig"), "application/octet-stream")).text();
      if (!verifyZip(zip, sig, p.publicKey)) bad.push(`${p.id}: plugin.zip.sig does not verify with the pinned key`);
      else console.log(`ok  ${p.id}: latest release verifies`);
    } catch (e) {
      bad.push(`${p.id}: ${e.message}`);
    }
  }
  return bad;
}

const args = process.argv.slice(2);
const file = args.find((a) => !a.startsWith("--"));
if (!file) { console.error("usage: node registry/check.mjs registry.json [--online]"); process.exit(2); }
const text = readFileSync(file, "utf8");
let problems = checkRegistry(text);
if (!problems.length && args.includes("--online")) problems = await online(JSON.parse(text).plugins);
if (problems.length) { for (const p of problems) console.error("error: " + p); process.exit(1); }
console.log(`${file}: ok (${JSON.parse(text).plugins.length} plugins)`);
