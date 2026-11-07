# S1 EPUB corpus

Public-domain EPUBs for crengine rendering and the CFI conformance spike.
None of these come from Pavel's Zotero library.

| File | Source | Why it's here |
|---|---|---|
| pride-and-prejudice.epub | https://standardebooks.org/ebooks/jane-austen/pride-and-prejudice | EPUB 3 with nav, clean modern markup, inline `abbr`/`em` |
| moby-dick-se.epub | https://standardebooks.org/ebooks/herman-melville/moby-dick | long book, many sections, endnotes |
| origin-of-species-se.epub | https://standardebooks.org/ebooks/charles-darwin/the-origin-of-species | scientific prose, tables, endnotes |
| alice-gutenberg-epub3.epub | https://www.gutenberg.org/ebooks/11 (epub3.images) | Gutenberg markup, hard-wrapped lines inside paragraphs, images |
| frankenstein-epub2-ncx.epub | https://www.gutenberg.org/ebooks/84 (epub.noimages) | EPUB 2, NCX only, no nav document |
| waste-land-poetry.epub | https://www.gutenberg.org/ebooks/1321 | poetry: line breaks, indentation, notes |
| gibbon-footnotes.epub | https://www.gutenberg.org/ebooks/25717 (epub.noimages) | heavy footnotes |
| the-prince-notes.epub | https://www.gutenberg.org/ebooks/1232 | Gutenberg EPUB 3, editor notes and footnote links (replaces a 13 MB maths book Zotero's reader could not open headless) |
| beeton-tables.epub | https://www.gutenberg.org/ebooks/10136 (epub.noimages) | very long, many tables |
| problems-1001-ru.epub | https://www.gutenberg.org/ebooks/16527 | Russian («1001 задача для умственного счета»), numbered problems |

Licences: Standard Ebooks productions are CC0; Project Gutenberg texts are public domain in the US
(the Gutenberg licence text inside each file applies to redistribution with the trademark).

## CFI reference data (`tests/data/cfi/*.zotero.json`)

For each book above: test ranges selected and annotated by Zotero 11.0's own reader (run headless
in spike S1, `spikes/s1-cfi/harness`): Zotero's `cfi`, `sortIndex` and the selected `text`. No
Zotero library was involved; the books are public domain.

## Generated fixtures

`no-toc.epub` (two spine items, no NCX/nav), `drm.epub` (META-INF/encryption.xml with AES on a
content file) and `broken.epub` (the first third of waste-land-poetry.epub) are made by the
project for error-handling tests.

`links-patterns.epub` (made by `make_links_epub.py`): the link patterns real EPUBs use —
Gutenberg notes (an empty anchor paragraph before `<p class="foot">N (return) [ … ]`), Pandoc
notes with a back-link, a Standard Ebooks endnote, a chapter link and a printed-page anchor.
