# links-patterns.epub: link patterns real EPUBs use, for the footnote/link tests. Run:
#   python3 tests/data/epub/make_links_epub.py
import zipfile

CH1 = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>One</title></head><body>
<h2 id="one">Chapter One</h2>
<p>Gutenberg marks a note like this<a href="ch1.xhtml#linknote-26" id="linknoteref-26" class="pginternal">26</a> in the text.</p>
<p>Pandoc marks one like this<a href="#fn1" class="footnote-ref" id="fnref1" role="doc-noteref"><sup>1</sup></a> instead.</p>
<p>Standard Ebooks: an endnote<a href="ch2.xhtml#note-1" id="noteref-1" epub:type="noteref">1</a> at the back.</p>
<p>See the second chapter, <a href="ch2.xhtml#two">II</a>, and the printed page <a href="ch1.xhtml#Page_23">23</a>.</p>
<p><a id="Page_23"></a>This paragraph begins printed page 23 and is long enough to be ordinary text, not a note at all.</p>
<p><a id="linknote-26"><!-- Note --></a></p>
<p class="foot">26 (<a href="ch1.xhtml#linknoteref-26" class="pginternal">return</a>)<br/> [ The senators were obliged to hold a third of their land in Italy.]</p>
<p><a id="linknote-27"><!-- Note --></a></p>
<p class="foot">27 (<a href="ch1.xhtml#linknoteref-27" class="pginternal">return</a>)<br/> [ Another note, not the one asked for.]</p>
<section class="footnotes" role="doc-endnotes"><ol>
<li id="fn1"><p>Pandoc's note text.<a href="#fnref1" class="footnote-back" role="doc-backlink">↩︎</a></p></li>
</ol></section>
</body></html>"""

CH2 = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Two</title></head><body>
<h2 id="two">Chapter Two</h2>
<p>The second chapter.</p>
<section epub:type="endnotes"><h2>Endnotes</h2><ol>
<li id="note-1" epub:type="endnote"><p>An endnote at the back. <a href="ch1.xhtml#noteref-1" epub:type="backlink">↩</a></p></li>
</ol></section>
</body></html>"""

OPF = """<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">links-patterns</dc:identifier>
<dc:title>Link patterns</dc:title><dc:language>en</dc:language></metadata>
<manifest><item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>
<item id="ch2" href="ch2.xhtml" media-type="application/xhtml+xml"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine><itemref idref="ch1"/><itemref idref="ch2"/></spine></package>"""

NAV = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Nav</title></head><body>
<nav epub:type="toc"><ol><li><a href="ch1.xhtml">One</a></li><li><a href="ch2.xhtml">Two</a></li></ol></nav></body></html>"""

CONTAINER = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>"""

with zipfile.ZipFile("tests/data/epub/links-patterns.epub", "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
    for name, text in [("META-INF/container.xml", CONTAINER), ("content.opf", OPF), ("nav.xhtml", NAV),
                       ("ch1.xhtml", CH1), ("ch2.xhtml", CH2)]:
        z.writestr(name, text, compress_type=zipfile.ZIP_DEFLATED)
