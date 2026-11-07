"""Makes rotated.pdf (one page, /Rotate 90, real text) and scan.pdf (one page, an image only).

uv run --with reportlab --with pypdf --with pillow python tests/data/pdf/make_rotated_scan.py
"""
import io
import pathlib

from PIL import Image, ImageDraw
from pypdf import PdfReader, PdfWriter
from reportlab.lib.pagesizes import letter
from reportlab.pdfgen import canvas

here = pathlib.Path(__file__).parent

buf = io.BytesIO()
c = canvas.Canvas(buf, pagesize=letter)
c.setFont("Helvetica", 14)
c.drawString(72, 700, "Rotated page. The Hamiltonian is convex here.")
for i in range(10):
    c.drawString(72, 660 - 20 * i, f"Line {i} of the rotated page: lorem ipsum dolor sit amet.")
c.showPage()
c.save()
reader = PdfReader(io.BytesIO(buf.getvalue()))
writer = PdfWriter()
page = reader.pages[0]
page.rotate(90)
writer.add_page(page)
with open(here / "rotated.pdf", "wb") as f:
    writer.write(f)

img = Image.new("L", (850, 1100), 255)
d = ImageDraw.Draw(img)
for i in range(30):
    d.rectangle([80, 100 + 30 * i, 760, 112 + 30 * i], fill=40)  # lines of "text" drawn as pixels
png = io.BytesIO()
img.save(png, format="PNG")
png.seek(0)
c = canvas.Canvas(str(here / "scan.pdf"), pagesize=letter)
from reportlab.lib.utils import ImageReader
c.drawImage(ImageReader(png), 0, 0, width=letter[0], height=letter[1])
c.showPage()
c.save()
