# links.pdf: citations as real papers have them. Run:
#   uv run --with reportlab python -u tests/data/pdf/make_links.py
# page 1: cites [1] (a destination exactly at the entry), a web link, [FV07] and [Smith 2020]
#         (destinations one line above the entry, as hyperref/natbib put them), and [B2]
#         (a reference in the right column of a two-column list).
# page 2: a numbered list. page 3: an alphabetic-label list with hanging indents. page 4: a
# two-column list, and a vertical arXiv stamp in the margin.
from reportlab.lib.pagesizes import letter
from reportlab.pdfgen import canvas

c = canvas.Canvas("tests/data/pdf/links.pdf", pagesize=letter)
H = "Helvetica"


def link_at(text_before, label, y, dest):
    w = c.stringWidth(text_before, H, 12)
    c.linkAbsolute(label, dest, (72 + w, y - 3, 72 + w + c.stringWidth(label, H, 12), y + 11))


c.setFont(H, 12)
c.drawString(72, 700, "Physics-informed networks were introduced in [1] and are")
link_at("Physics-informed networks were introduced in ", "[1]", 700, "ref1")
c.drawString(72, 684, "described at https://example.org/pinns in more detail.")
w2 = c.stringWidth("described at ", H, 12)
c.linkURL("https://example.org/pinns", (72 + w2, 681, 72 + w2 + c.stringWidth("https://example.org/pinns", H, 12), 695))
c.drawString(72, 650, "Rough paths [FV07] and the survey [Smith 2020] are related.")
link_at("Rough paths ", "[FV07]", 650, "fv07")
link_at("Rough paths [FV07] and the survey ", "[Smith 2020]", 650, "smith")
c.drawString(72, 620, "Two-column lists: see [B2].")
link_at("Two-column lists: see ", "[B2]", 620, "b2")
c.showPage()

c.setFont("Helvetica-Bold", 14)
c.drawString(72, 720, "References")
c.setFont(H, 12)
c.bookmarkPage("ref1", fit="XYZ", left=72, top=700)
c.drawString(72, 690, "[1] M. Raissi, P. Perdikaris, G. E. Karniadakis. Physics-informed neural")
c.drawString(90, 674, "networks. J. Comput. Phys. 378 (2019), 686-707.")
c.drawString(72, 640, "[2] Another reference that is not the one cited.")
c.showPage()

# Alphabetic labels and author-year, hanging indents (continuations at x=90), no gap between
# entries; the destination is one line above each entry, at the previous line's baseline, as
# hyperref puts it, so the previous entry's last line is right there.
c.setFont(H, 12)
y = 700
entries = [
    ("[Dud89]", ["[Dud89] R. M. Dudley. Real Analysis and Probability. Wadsworth, 1989."], None),
    ("[FV07]", ["[FV07] P. Friz and N. Victoir. Multidimensional Stochastic Processes as Rough",
                "Paths. Cambridge University Press, 2010."], "fv07"),
    ("[Hai14]", ["[Hai14] M. Hairer. A theory of regularity structures. Invent. Math. 198 (2014),",
                 "269-504."], None),
    ("Smith", ["Smith, J. (2020). A survey of rough paths. Ann. Probab. 48, 1-40."], "smith"),
    ("Taylor", ["Taylor, K. (2021). After the survey. Bernoulli 27, 2-9."], None),
]
for label, lines, dest in entries:
    if dest:
        c.bookmarkPage(dest, fit="XYZ", left=72, top=y + 15)  # the previous line's baseline
    for i, line in enumerate(lines):
        c.drawString(72 if i == 0 else 90, y, line)
        y -= 15
c.showPage()

# Two columns; a vertical stamp in the left margin.
c.setFont(H, 10)
left = ["[A1] First left entry about one", "thing in the left column.", "[A2] Second left entry, also left."]
right = ["[B1] First right entry.", "[B2] The right column entry we", "cite, continued here.", "[B3] Third right entry."]
for i, line in enumerate(left):
    c.drawString(72, 700 - i * 13, line)
for i, line in enumerate(right):
    if line.startswith("[B2]"):
        c.bookmarkPage("b2", fit="XYZ", left=320, top=700 - i * 13 + 11)
    c.drawString(320, 700 - i * 13, line)
c.saveState()
c.translate(30, 300)
c.rotate(90)
c.drawString(0, 0, "arXiv:2101.00001v1 [math.PR] 1 Jan 2021")
c.restoreState()
c.showPage()
c.save()
