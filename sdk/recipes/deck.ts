// A deck: one card at a time, swipe left to dismiss, right to keep. Opens from the Plugins
// launcher on Home. No permission needed.
// plugin.json: "permissions": []

const cards = ["Gaussian random fields", "Brownian sheets", "Stochastic PDEs"];
let kept: string[] = [];
let message = "";

zk.screen("deck", {
  title: "Cards",
  main: true,
  render: () =>
    zk.ui.column([
      zk.ui.deck({
        id: "card",
        card: cards[0] ? zk.ui.card({ title: cards[0], meta: `${cards.length} left` }) : null,
        empty: `All done. Kept ${kept.length}.`,
      }),
      zk.ui.buttons([
        { id: "drop", title: "Drop" },
        { id: "keep", title: "Keep", primary: true },
      ]),
      message && zk.ui.notice(message),
    ]),
  onEvent: (e) => {
    const verdict =
      e.kind === "swipe" ? (e.direction === "right" ? "keep" : "drop") : e.kind === "tap" ? e.id : null;
    if (!verdict || cards.length === 0) return;
    const card = cards.shift()!;
    if (verdict === "keep") kept.push(card);
    message = `${verdict === "keep" ? "Kept" : "Dropped"} “${card}”`;
  },
});
