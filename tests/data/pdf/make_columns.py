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


# Also: a contents entry "Section 2.3" at page 2, right column, line 30 (x and y), and on page 1
# a link (left column, line 5) to page 3, left column, line 40, given by its height only (FitH).
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
    if page == 2:
        c.bookmarkPage("sec23", fit="XYZ", left=318, top=H - 72 - 30 * LEAD + 8)
        c.addOutlineEntry("Section 2.3", "sec23", level=0)
    if page == 3:
        c.bookmarkPage("l40", fit="FitH", top=H - 72 - 40 * LEAD + 8)
    if page == 1:
        c.linkAbsolute("to L3-40", "l40", (54, H - 72 - 5 * LEAD - 2, 284, H - 72 - 5 * LEAD + 8))
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

# mirrored.pdf: a book with mirrored margins, 6 pages of 432 x 648 pt: odd pages' text at
# x 51-328, even pages' at x 95-372 (the inner margin is wider). Each line starts "M<page>-<line>"
# and ends "E<page>-<line>" so a cut at either edge shows. Pages 1-5 have 34 lines; page 6 runs
# lower, 40 lines, as a page with a long footnote does.
PW, PH = 432, 648
c = canvas.Canvas("tests/data/pdf/mirrored.pdf", pagesize=(PW, PH))
for page in range(1, 7):
    x = 51 if page % 2 else 95
    c.setFont(FONT, 9)
    lines = 40 if page == 6 else 34
    for line in range(1, lines + 1):
        head, tail = f"M{page}-{line:02d} ", f" E{page}-{line:02d}"
        c.drawString(x, PH - 50 - line * 13.5, head + filler(51 - len(head) - len(tail)) + tail)
    c.showPage()
c.save()
