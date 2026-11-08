# publisher-fonts.epub: a book that names its own fonts, as most real EPUBs do (Calibre and
# publishers: "Times New Roman", serif; Arial for some paragraphs; a monospace code block).
# The reader's font choice must win over them. Run:  python3 tests/data/epub/make_styled_epub.py
import zipfile

WORDS = ("the reader chooses a font and every paragraph of the book should follow it whatever "
         "the publisher wrote in the style sheet about times and arial and other families ").split()
PARA = " ".join(WORDS * 3)

CSS = """body { font-family: "Times New Roman", serif; }
p.sans { font-family: Arial, Helvetica, sans-serif; }
pre { font-family: "Courier New", monospace; }"""

CH = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>One</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head><body>
<h2>Chapter One</h2>
""" + "".join(f"<p>{PARA}</p>\n<p class=\"sans\">{PARA}</p>\n" for _ in range(6)) + """
<pre>int main() { return 0; }</pre>
</body></html>"""

OPF = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">publisher-fonts</dc:identifier>
<dc:title>Publisher fonts</dc:title><dc:language>en</dc:language></metadata>
<manifest><item id="ch" href="ch.xhtml" media-type="application/xhtml+xml"/>
<item id="css" href="style.css" media-type="text/css"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine><itemref idref="ch"/></spine></package>"""

NAV = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Nav</title></head>
<body><nav epub:type="toc"><ol><li><a href="ch.xhtml">One</a></li></ol></nav></body></html>"""

with zipfile.ZipFile("tests/data/epub/publisher-fonts.epub", "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
    z.writestr("META-INF/container.xml", """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>""")
    z.writestr("content.opf", OPF)
    z.writestr("nav.xhtml", NAV)
    z.writestr("ch.xhtml", CH)
    z.writestr("style.css", CSS)
