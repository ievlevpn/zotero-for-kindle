# links.pdf: page 1 cites [1] (an internal link to the reference on page 2) and a web link;
# page 2 holds the reference list. Run: uv run --with reportlab python -u tests/data/pdf/make_links.py
from reportlab.lib.pagesizes import letter
from reportlab.pdfgen import canvas

c = canvas.Canvas("tests/data/pdf/links.pdf", pagesize=letter)
c.setFont("Helvetica", 12)
c.drawString(72, 700, "Physics-informed networks were introduced in [1] and are")
c.drawString(72, 684, "described at https://example.org/pinns in more detail.")
w = c.stringWidth("Physics-informed networks were introduced in ", "Helvetica", 12)
cite = (72 + w, 697, 72 + w + c.stringWidth("[1]", "Helvetica", 12), 711)
c.linkAbsolute("[1]", "ref1", cite)
w2 = c.stringWidth("described at ", "Helvetica", 12)
url = (72 + w2, 681, 72 + w2 + c.stringWidth("https://example.org/pinns", "Helvetica", 12), 695)
c.linkURL("https://example.org/pinns", url)
c.showPage()
c.setFont("Helvetica-Bold", 14)
c.drawString(72, 720, "References")
c.setFont("Helvetica", 12)
c.bookmarkPage("ref1", fit="XYZ", left=72, top=700)
c.drawString(72, 690, "[1] M. Raissi, P. Perdikaris, G. E. Karniadakis. Physics-informed neural")
c.drawString(90, 674, "networks. J. Comput. Phys. 378 (2019), 686-707.")
c.drawString(72, 640, "[2] Another reference that is not the one cited.")
c.showPage()
c.save()
