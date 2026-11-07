# columns.pdf and single-column.pdf: papers laid out as journals and preprints are, for column view.
# Run:  uv run --with reportlab python -u tests/data/pdf/make_columns.py
# columns.pdf: 4 letter pages, two justified columns (Courier, so every line ends at one x) of 60
# lines each, a running header at the top and a page number at the bottom. Each line starts with
# its place: "L2-07" is page 2 (1-based), left column, line 7; "R2-07" the right column's.
# single-column.pdf: 3 letter pages, one column of 50 lines, each starting "S2-07"; some lines
# short and indented (displayed equations), as in maths papers.
from reportlab.lib.pagesizes import letter
from reportlab.pdfgen import canvas

FONT, SIZE, LEAD = "Courier", 8, 10.5
W, H = letter


def filler(n):
    words = "the process converges in law to a stable limit as the scale grows and the jumps".split()
    out, i = "", 0
    while len(out) < n:
        out += words[i % len(words)] + " "
        i += 1
    return out[:n]


c = canvas.Canvas("tests/data/pdf/columns.pdf", pagesize=letter)
for page in range(1, 5):
    c.setFont("Helvetica", 7)
    c.drawString(54, H - 40, "Journal of Examples 12 (2026)")
    c.drawCentredString(W / 2, 30, str(page))
    c.setFont(FONT, SIZE)
    for side, x in (("L", 54), ("R", 318)):
        for line in range(1, 61):
            label = f"{side}{page}-{line:02d} "
            c.drawString(x, H - 72 - line * LEAD, label + filler(48 - len(label)))
    c.showPage()
c.save()

c = canvas.Canvas("tests/data/pdf/single-column.pdf", pagesize=letter)
for page in range(1, 4):
    c.setFont(FONT, 10)
    for line in range(1, 51):
        label = f"S{page}-{line:02d} "
        if line % 9 == 0:  # a displayed equation, centred and short
            c.drawString(250, H - 72 - line * 13, label + "f(x) = g(x)")
        else:
            c.drawString(72, H - 72 - line * 13, label + filler(78 - len(label)))
    c.showPage()
c.save()
