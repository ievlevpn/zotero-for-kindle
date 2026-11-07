"""Draws the launcher cover shown in the Kindle library: assets/launcher/icon.png.

The library shows covers as portrait tiles (3:4, like the screen); a square image is cut at
the sides, so the cover is 3:4 with everything well inside the edges.
uv run --with pillow python tools/make-cover.py
"""
import pathlib

from PIL import Image, ImageDraw, ImageFont

root = pathlib.Path(__file__).resolve().parent.parent
fonts = root / "assets" / "fonts-device"
W, H = 600, 800
img = Image.new("L", (W, H), 255)
d = ImageDraw.Draw(img)


def centred(text, font, y, fill):
    box = d.textbbox((0, 0), text, font=font)
    d.text(((W - (box[2] - box[0])) / 2 - box[0], y - box[1]), text, font=font, fill=fill)


# The mark: a large white Z on a dark block.
d.rectangle([0, 0, W, 500], fill=34)
z = ImageFont.truetype(str(fonts / "Amazon-Ember-Heavy.ttf"), 380)
box = d.textbbox((0, 0), "Z", font=z)
centred("Z", z, (500 - (box[3] - box[1])) / 2, 255)

# The name, large enough to read on a small tile.
centred("Zotero", ImageFont.truetype(str(fonts / "Amazon-Ember-Bold.ttf"), 112), 560, 0)
d.rectangle([W / 2 - 60, 706, W / 2 + 60, 710], fill=150)
centred("for Kindle", ImageFont.truetype(str(fonts / "Amazon-Ember-Regular.ttf"), 46), 732, 80)

img.save(root / "assets" / "launcher" / "icon.png", optimize=True)
