// examples/explain-with-ai/src/main.ts
var MAX_PASSAGE = 4e3;
zk.selectionAction({
  id: "explain",
  title: "Explain",
  run: async ({ text, item, pageLabel }, again) => {
    if (again?.action === "note") {
      const where = pageLabel ? ` (p. ${pageLabel})` : "";
      const html = [`<p><b>Explained${where}:</b> ${escapeHtml(passageOf(text))}</p>`, ...paragraphs(again.text)].join("");
      await zk.zotero.addNote(item.key, html);
      return "Saved as a note on this book.";
    }
    const { apiKey, model = "claude-sonnet-5-5", language, maxTokens = 600 } = zk.settings;
    if (!apiKey) throw new Error('Add your Anthropic API key to settings.json: { "apiKey": "sk-ant-\u2026" }');
    const passage = passageOf(text);
    const prompt = [
      `I am reading "${item.title}"${item.year ? ` (${item.year})` : ""}.`,
      "Explain this passage briefly and plainly: what it says, and any term or step a reader might miss.",
      language ? `Answer in ${language}.` : "Answer in the language of the passage.",
      "",
      passage
    ].join("\n");
    const res = await zk.fetch("https://api.anthropic.com/v1/messages", {
      method: "POST",
      headers: {
        "x-api-key": apiKey,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json"
      },
      body: JSON.stringify({
        model,
        max_tokens: maxTokens,
        messages: [{ role: "user", content: prompt }]
      })
    });
    if (!res.ok) {
      const reply2 = await res.json().catch(() => ({}));
      throw new Error(`Anthropic: ${reply2.error?.message ?? `HTTP ${res.status}`}`);
    }
    const reply = await res.json();
    const answer = reply.content.find((block) => block.type === "text")?.text?.trim();
    if (!answer) throw new Error("Anthropic: the reply had no text");
    return { text: answer, actions: [{ id: "note", title: "Save as note" }] };
  }
});
function passageOf(text) {
  return text.length > MAX_PASSAGE ? `${text.slice(0, MAX_PASSAGE)}\u2026` : text;
}
function paragraphs(text) {
  return text.split(/\n\s*\n/).map((p) => `<p>${escapeHtml(p.trim()).replace(/\n/g, "<br>")}</p>`);
}
function escapeHtml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
