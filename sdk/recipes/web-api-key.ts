// Translate each highlight you make on the Kindle with DeepL, using your own API key, and keep
// the translations in a Markdown file.
// plugin.json: "permissions": ["annotations:read", "net:api-free.deepl.com"]
// settings.json (next to plugin.json): { "deeplKey": "your key", "targetLang": "EN-GB" }

// Describe settings.json, so zk.settings is typed.
declare global {
  namespace zk {
    interface Settings {
      deeplKey?: string;
      targetLang?: string;
    }
  }
}

zk.on("highlightCreated", async ({ origin, annotations }) => {
  if (origin !== "kindle") return;
  const key = zk.settings.deeplKey;
  if (!key) throw new Error('Put your DeepL key in settings.json: { "deeplKey": "…" }');
  const texts = annotations.flatMap((a) => (a.text ? [a.text] : []));
  if (texts.length === 0) return;

  const res = await zk.fetch("https://api-free.deepl.com/v2/translate", {
    method: "POST",
    headers: { "content-type": "application/json", authorization: `DeepL-Auth-Key ${key}` },
    body: JSON.stringify({ text: texts, target_lang: zk.settings.targetLang ?? "EN-GB" }),
    timeout: 4000, // the handler has 5 seconds in all
  });
  if (!res.ok) throw new Error(`DeepL: HTTP ${res.status}`);
  const { translations } = await res.json<{ translations: { text: string }[] }>();

  // Keep them in storage (update: safe if two highlights come in at once), then rewrite the file.
  const all = await zk.storage.update<string[]>("translations", (old = []) => [
    ...old,
    ...translations.map((t, i) => `> ${texts[i]}\n\n${t.text}\n`),
  ]);
  await zk.files.write("translations.md", (all ?? []).join("\n"));
});
