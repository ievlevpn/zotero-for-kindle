// Explain with AI — select a passage in a book, choose "Explain", read Claude's explanation.
//
// The plugin adds one action to the selection bar (under More…). When the user picks it, the
// app hands `run` the selected text, the item being read and the file; `run` asks Anthropic's
// Messages API and returns the answer with one button, "Save as note". The app shows it in its
// result pop-up. Tapping the button runs the action again, with `again` saying which button and
// what the pop-up showed: the answer is saved as a child note of the book, and the pop-up says so.
//
// Permissions (plugin.json): "net:api.anthropic.com" — the one host it talks to — and
// "zotero:notes" for "Save as note". The selection and its item are handed over because the user
// chose the action, so it needs no "library:read".

// settings.json, next to plugin.json, edited over USB:
//   { "apiKey": "sk-ant-…", "model": "claude-sonnet-5-5", "language": "English" }
declare global {
  namespace zk {
    interface Settings {
      /** An Anthropic API key (console.anthropic.com). Required. */
      apiKey?: string;
      /** Default `claude-sonnet-5-5`. */
      model?: string;
      /** Answer in this language; default: the language of the passage. */
      language?: string;
      /** Longest answer, in tokens. Default 600: a pop-up on a Kindle is small. */
      maxTokens?: number;
    }
  }
}

/** Longest passage sent, in characters; a selection can run over pages. */
const MAX_PASSAGE = 4000;

/** The part of the Messages API reply this plugin reads. */
interface MessagesReply {
  content: { type: string; text?: string }[];
}

/** An error reply: `{ "type": "error", "error": { "type": "…", "message": "…" } }`. */
interface ErrorReply {
  error?: { message?: string };
}

zk.selectionAction({
  id: "explain",
  title: "Explain",
  run: async ({ text, item, pageLabel }, again) => {
    // "Save as note": the pop-up's text comes back with the tap, so nothing is asked twice.
    if (again?.action === "note") {
      const where = pageLabel ? ` (p. ${pageLabel})` : "";
      const html = [`<p><b>Explained${where}:</b> ${escapeHtml(passageOf(text))}</p>`, ...paragraphs(again.text)].join("");
      await zk.zotero.addNote(item.key, html);
      return "Saved as a note on this book."; // replaces the pop-up's text; no buttons this time
    }

    const { apiKey, model = "claude-sonnet-5-5", language, maxTokens = 600 } = zk.settings;
    // A thrown error is shown to the user as the action having failed, with its message.
    if (!apiKey) throw new Error('Add your Anthropic API key to settings.json: { "apiKey": "sk-ant-…" }');

    const passage = passageOf(text);
    const prompt = [
      `I am reading "${item.title}"${item.year ? ` (${item.year})` : ""}.`,
      "Explain this passage briefly and plainly: what it says, and any term or step a reader might miss.",
      language ? `Answer in ${language}.` : "Answer in the language of the passage.",
      "",
      passage,
    ].join("\n");

    // zk.fetch is the web's fetch with limits: HTTPS only, to the hosts in plugin.json, no
    // cookies. HTTP errors don't reject — check `ok`.
    const res = await zk.fetch("https://api.anthropic.com/v1/messages", {
      method: "POST",
      headers: {
        "x-api-key": apiKey,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
      },
      body: JSON.stringify({
        model,
        max_tokens: maxTokens,
        messages: [{ role: "user", content: prompt }],
      }),
    });
    if (!res.ok) {
      const reply = await res.json<ErrorReply>().catch((): ErrorReply => ({}));
      throw new Error(`Anthropic: ${reply.error?.message ?? `HTTP ${res.status}`}`);
    }

    // The answer is the text block of the reply (content[0] for a plain text answer).
    const reply = await res.json<MessagesReply>();
    const answer = reply.content.find((block) => block.type === "text")?.text?.trim();
    if (!answer) throw new Error("Anthropic: the reply had no text");
    // The pop-up shows `text`; each of `actions` (at most 2) is a button beside Close.
    return { text: answer, actions: [{ id: "note", title: "Save as note" }] };
  },
});

function passageOf(text: string): string {
  return text.length > MAX_PASSAGE ? `${text.slice(0, MAX_PASSAGE)}…` : text;
}

/** The answer's paragraphs (blank-line separated) as note HTML. */
function paragraphs(text: string): string[] {
  return text.split(/\n\s*\n/).map((p) => `<p>${escapeHtml(p.trim()).replace(/\n/g, "<br>")}</p>`);
}

function escapeHtml(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
