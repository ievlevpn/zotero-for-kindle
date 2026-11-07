# Fast Font (Born2Root, MIT): give each family one readable name, as the font picker shows it.
import sys
from fontTools.ttLib import TTFont
src, dst, family, style = sys.argv[1:5]
f = TTFont(src)
name = f["name"]
ps = family.replace(" ", "") + "-" + style.replace(" ", "")
full = family if style == "Regular" else f"{family} {style}"
for rec in list(name.names):
    if rec.nameID in (16, 17, 21, 22):
        name.removeNames(nameID=rec.nameID)
for nid, val in ((1, family), (2, style), (3, ps), (4, full), (6, ps)):
    name.setName(val, nid, 3, 1, 0x409)
    name.setName(val, nid, 1, 0, 0)
f.save(dst)
