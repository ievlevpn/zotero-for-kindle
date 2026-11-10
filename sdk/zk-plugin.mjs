#!/usr/bin/env node
// The authors' tool for publishing a plugin to the catalog. No dependencies beyond Node 22.
//
//   node zk-plugin.mjs keygen [--out zk-plugin-key.json]   make your signing key (once)
//   node zk-plugin.mjs pack [dir] [--no-build]             build, then zip dist/<id>/ to dist/plugin.zip
//   node zk-plugin.mjs sign [dir] [--key zk-plugin-key.json]  write dist/plugin.zip.sig
//   node zk-plugin.mjs self-test                           check the tool itself (sdk/check.sh)
//
// The signature is Ed25519 over the zip's exact bytes, as base64: what the app verifies.
// zk-plugin-key.json holds your private key. Keep it off CI and out of git.
import { createPrivateKey, createPublicKey, generateKeyPairSync, sign, verify } from "node:crypto";
import { execFileSync } from "node:child_process";
import { existsSync, mkdtempSync, readdirSync, lstatSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { crc32, deflateRawSync } from "node:zlib";
import { fileURLToPath } from "node:url";

const MAX_ZIP = 8 << 20;
const MAX_TREE = 32 << 20;   // the unpacked tree, as the app limits it
const MAX_ENTRIES = 2000;
const KEY_FILE = "zk-plugin-key.json";
const here = fileURLToPath(new URL(".", import.meta.url));

class Refused extends Error {}
const refuse = (msg) => { throw new Refused(msg); };

// ---- keys -------------------------------------------------------------------------------------

// The raw 32-byte public key, base64: the form the registry pins.
function publicKeyBase64(jwk) {
  return Buffer.from(jwk.x, "base64url").toString("base64");
}

function keygen(out) {
  if (existsSync(out)) refuse(`${out} already exists; it is not overwritten. Move it away first, if you mean to start over.`);
  const { privateKey } = generateKeyPairSync("ed25519");
  const jwk = privateKey.export({ format: "jwk" });
  const body = { format: "zk-plugin-key-1", privateKey: jwk, publicKey: publicKeyBase64(jwk) };
  writeFileSync(out, JSON.stringify(body, null, 2) + "\n", { mode: 0o600, flag: "wx" });
  return body.publicKey;
}

function loadKey(file) {
  if (!existsSync(file)) refuse(`${file} not found. Make one with: node zk-plugin.mjs keygen`);
  const body = JSON.parse(readFileSync(file, "utf8"));
  return createPrivateKey({ key: body.privateKey, format: "jwk" });
}

// ---- zip --------------------------------------------------------------------------------------

// A deterministic zip (fixed timestamps, sorted names) of files = [[name, Buffer]].
function makeZip(files) {
  const parts = [], central = [];
  let offset = 0;
  for (const [name, data] of files) {
    const nameBuf = Buffer.from(name, "utf8");
    const deflated = deflateRawSync(data);
    const stored = deflated.length >= data.length;
    const body = stored ? data : deflated;
    const crc = crc32(data) >>> 0;
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);  // UTF-8 names
    local.writeUInt16LE(stored ? 0 : 8, 8);
    local.writeUInt16LE(0, 10);       // time 00:00
    local.writeUInt16LE(0x21, 12);    // date 1980-01-01
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(body.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(nameBuf.length, 26);
    parts.push(local, nameBuf, body);
    const c = Buffer.alloc(46);
    c.writeUInt32LE(0x02014b50, 0);
    c.writeUInt16LE(20, 4);
    c.writeUInt16LE(20, 6);
    c.writeUInt16LE(0x0800, 8);
    c.writeUInt16LE(stored ? 0 : 8, 10);
    c.writeUInt16LE(0, 12);
    c.writeUInt16LE(0x21, 14);
    c.writeUInt32LE(crc, 16);
    c.writeUInt32LE(body.length, 20);
    c.writeUInt32LE(data.length, 24);
    c.writeUInt16LE(nameBuf.length, 28);
    c.writeUInt32LE((0o100644 << 16) >>> 0, 38);
    c.writeUInt32LE(offset, 42);
    central.push(c, nameBuf);
    offset += 30 + nameBuf.length + body.length;
  }
  const dir = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(files.length, 8);
  end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(dir.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...parts, dir, end]);
}

// Every file under root as [relative/name, Buffer]. Refuses data/, dot-files and links.
function collect(root, rel = "") {
  const found = [];
  for (const name of readdirSync(join(root, rel)).sort()) {
    const path = rel ? `${rel}/${name}` : name;
    if (name.startsWith(".")) refuse(`${path}: dot-files are not published; remove it from dist/.`);
    if (/^data[. ]*$/i.test(name)) refuse(`${path}: a data/ folder is the user's, and never part of a plugin.`);
    const st = lstatSync(join(root, path));
    if (st.isSymbolicLink()) refuse(`${path}: links are not allowed.`);
    if (st.isDirectory()) found.push(...collect(root, path));
    else found.push([path, readFileSync(join(root, path))]);
  }
  return found;
}

function pack(dir, build) {
  const manifestPath = join(dir, "plugin.json");
  if (!existsSync(manifestPath)) refuse(`${manifestPath} not found: run this in your plugin's folder.`);
  const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
  if (build) {
    if (!existsSync(join(dir, "build.mjs"))) refuse("no build.mjs here; build yourself and pass --no-build.");
    if (typeof manifest.id === "string" && /^[a-z0-9]+(-[a-z0-9]+)*$/.test(manifest.id))
      rmSync(join(dir, "dist", manifest.id), { recursive: true, force: true });  // nothing stale is shipped
    execFileSync(process.execPath, ["build.mjs"], { cwd: dir, stdio: "inherit" });
  }
  const root = join(dir, "dist", manifest.id);
  if (!existsSync(join(root, "plugin.json"))) refuse(`${root}/plugin.json not found: the build should produce dist/${manifest.id}/.`);
  checkManifest(join(root, "plugin.json"), manifest.id);
  const files = collect(root);
  if (files.length > MAX_ENTRIES) refuse(`${files.length} files; the app takes at most ${MAX_ENTRIES}.`);
  const tree = files.reduce((n, [, data]) => n + data.length, 0);
  if (tree > MAX_TREE) refuse(`the plugin is ${tree} bytes unpacked; the app takes at most ${MAX_TREE}.`);
  const zip = makeZip(files);
  if (zip.length > MAX_ZIP) refuse(`plugin.zip is ${zip.length} bytes; the catalog takes at most ${MAX_ZIP}.`);
  const out = join(dir, "dist", "plugin.zip");
  writeFileSync(out, zip);
  return { out, size: zip.length };
}

// What the app checks of the shipped plugin.json: the schema (validate-manifest.mjs, when the SDK's
// npm packages are installed), and always its id and an X.Y.Z version (the release tag must equal it).
function checkManifest(path, id) {
  const m = JSON.parse(readFileSync(path, "utf8"));
  if (m.id !== id) refuse(`${path}: id "${m.id}" isn't "${id}".`);
  if (typeof m.version !== "string" || !/^\d+\.\d+\.\d+$/.test(m.version))
    refuse(`${path}: version "${m.version}" must be X.Y.Z (the catalog takes no pre-releases).`);
  if (existsSync(join(here, "node_modules", "ajv"))) {
    try {
      execFileSync(process.execPath, [join(here, "validate-manifest.mjs"), path], { stdio: "inherit" });
    } catch {
      refuse(`${path} doesn't match the manifest schema (see above).`);
    }
  } else {
    console.error(`note: plugin.json not checked against the schema (run npm install in ${here} for that).`);
  }
}

// ---- sign -------------------------------------------------------------------------------------

function signZip(dir, keyFile) {
  const zipPath = join(dir, "dist", "plugin.zip");
  if (!existsSync(zipPath)) refuse(`${zipPath} not found: run pack first.`);
  const zip = readFileSync(zipPath);
  const sig = sign(null, zip, loadKey(keyFile)).toString("base64");
  const out = zipPath + ".sig";
  writeFileSync(out, sig + "\n");
  return out;
}

// What the app does with a download: raw public key (base64), the zip's bytes, base64 signature.
export function verifyZip(zip, sigBase64, publicKeyB64) {
  const raw = Buffer.from(publicKeyB64, "base64");
  if (raw.length !== 32) return false;
  const spki = Buffer.concat([Buffer.from("302a300506032b6570032100", "hex"), raw]);
  const key = createPublicKey({ key: spki, format: "der", type: "spki" });
  return verify(null, zip, key, Buffer.from(sigBase64.trim(), "base64"));
}

// ---- self-test --------------------------------------------------------------------------------

function selfTest() {
  const tmp = mkdtempSync(join(tmpdir(), "zk-plugin-"));
  try {
    const ok = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); console.log("ok  " + what); };
    const throws = (fn, what) => { try { fn(); } catch (e) { if (e instanceof Refused) return ok(true, what); throw e; } ok(false, what); };

    const keyFile = join(tmp, KEY_FILE);
    const pub = keygen(keyFile);
    ok(Buffer.from(pub, "base64").length === 32, "keygen: a 32-byte public key");
    ok((lstatSync(keyFile).mode & 0o777) === 0o600, "keygen: the key file is mode 0600");
    throws(() => keygen(keyFile), "keygen: refuses to overwrite");

    const plugin = join(tmp, "p");
    const root = join(plugin, "dist", "demo");
    mkdirSync(root, { recursive: true });
    writeFileSync(join(plugin, "plugin.json"), '{"id":"demo"}');
    const demo = { id: "demo", name: "Demo", version: "1.0.0", api: 0, main: "main.js", permissions: [] };
    writeFileSync(join(root, "plugin.json"), JSON.stringify(demo));
    writeFileSync(join(root, "main.js"), "export {};\n".repeat(50));
    pack(plugin, false);
    signZip(plugin, keyFile);
    const zip = readFileSync(join(plugin, "dist", "plugin.zip"));
    const sig = readFileSync(join(plugin, "dist", "plugin.zip.sig"), "utf8");
    ok(verifyZip(zip, sig, pub), "sign: the signature verifies with the public key");
    ok(!verifyZip(Buffer.concat([zip, Buffer.from("x")]), sig, pub), "sign: a changed zip does not verify");
    ok(zip.readUInt32LE(0) === 0x04034b50 && zip.includes("plugin.json"), "pack: plugin.json is in the zip");
    const unzipped = execFileSync("unzip", ["-Z1", join(plugin, "dist", "plugin.zip")]).toString().trim().split("\n");
    ok(unzipped.join(",") === "main.js,plugin.json", "pack: files at the root of the zip");

    writeFileSync(join(root, "plugin.json"), JSON.stringify({ ...demo, version: "1.0.0-beta.1" }));
    throws(() => pack(plugin, false), "pack: refuses a version that isn't X.Y.Z");
    writeFileSync(join(root, "plugin.json"), JSON.stringify({ ...demo, id: "other" }));
    throws(() => pack(plugin, false), "pack: refuses a plugin.json whose id isn't the folder's");
    writeFileSync(join(root, "plugin.json"), JSON.stringify(demo));
    mkdirSync(join(root, "data"));
    throws(() => pack(plugin, false), "pack: refuses a data/ folder");
    rmSync(join(root, "data"), { recursive: true });
    writeFileSync(join(root, ".DS_Store"), "x");
    throws(() => pack(plugin, false), "pack: refuses dot-files");

    // The committed fixture (tests/data/catalog/) verifies, as the C++ test also checks.
    const fx = join(here, "..", "tests", "data", "catalog");
    if (existsSync(join(fx, "plugin.zip"))) {
      const pk = readFileSync(join(fx, "test-public-key.txt"), "utf8").trim();
      ok(verifyZip(readFileSync(join(fx, "plugin.zip")), readFileSync(join(fx, "plugin.zip.sig"), "utf8"), pk),
        "the committed fixture verifies");
    }
    console.log("zk-plugin: self-test passed");
  } finally {
    rmSync(tmp, { recursive: true, force: true });
  }
}

// ---- main -------------------------------------------------------------------------------------

if (resolve(process.argv[1] ?? "") === fileURLToPath(import.meta.url)) {
  const [cmd, ...rest] = process.argv.slice(2);
  const flag = (name, fallback) => { const i = rest.indexOf(name); return i >= 0 ? rest[i + 1] : fallback; };
  const positional = rest.filter((a, i) => !a.startsWith("--") && !(i > 0 && rest[i - 1].startsWith("--") && rest[i - 1] !== "--no-build"));
  const dir = resolve(positional[0] ?? ".");
  try {
    if (cmd === "keygen") {
      const out = resolve(flag("--out", KEY_FILE));
      const pub = keygen(out);
      console.log(`wrote ${out} (secret: keep it private, off CI, backed up)\npublic key: ${pub}`);
    } else if (cmd === "pack") {
      const { out, size } = pack(dir, !rest.includes("--no-build"));
      console.log(`packed ${out} (${size} bytes)`);
    } else if (cmd === "sign") {
      console.log(`signed ${signZip(dir, resolve(flag("--key", KEY_FILE)))}`);
    } else if (cmd === "self-test") {
      selfTest();
    } else {
      console.error("usage: node zk-plugin.mjs keygen | pack [dir] [--no-build] | sign [dir] [--key file] | self-test");
      process.exit(2);
    }
  } catch (e) {
    if (e instanceof Refused) { console.error("error: " + e.message); process.exit(1); }
    throw e;
  }
}
