// src/main.ts
var MAX_PASSAGE = 4e3;
zk.selectionAction({
  id: "explain",
  title: "Explain",
  run: async ({ text, item }) => {
    const { apiKey, model = "claude-sonnet-5-5", language, maxTokens = 600 } = zk.settings;
    if (!apiKey) throw new Error('Add your Anthropic API key to settings.json: { "apiKey": "sk-ant-\u2026" }');
    const passage = text.length > MAX_PASSAGE ? `${text.slice(0, MAX_PASSAGE)}\u2026` : text;
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
    return answer;
  }
});
