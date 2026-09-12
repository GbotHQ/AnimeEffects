#!/usr/bin/env python3
"""Builds fig17_margin_parity.png: the 2x over-blur and its fix, on real artwork.

Inputs are the PNGs the probe saves with AE_EVAL_SAVE=1 from
    verify_mipmap --eval <csv> ~/Downloads/kemono_psd_1/000049.psd <dir>
run once before the margin fix and once after. The window is the one pixels differ in most.
"""
import sys
from PIL import Image, ImageDraw

before_fix, after_fix, out = sys.argv[1], sys.argv[2], sys.argv[3]
CROP, SCALE = 44, 7          # 44x44 canvas pixels of a 4500x4500 render, 7x nearest
TOPLEFT = (1940, 1440)       # the thin bright line the pre-fix AFTER smeared

panels = [
    ("ground truth (supersampled)", f"{before_fix}/000049_z500_ref.png"),
    ("BEFORE: level 0 only",        f"{before_fix}/000049_z500_before.png"),
    ("AFTER pre-fix: mipmapped",    f"{before_fix}/000049_z500_after.png"),
    ("AFTER post-fix: mipmapped",   f"{after_fix}/000049_z500_after.png"),
]

w = CROP * SCALE
canvas = Image.new("RGB", (w * len(panels) + 8 * (len(panels) - 1), w + 34), (24, 24, 24))
draw = ImageDraw.Draw(canvas)
for i, (label, path) in enumerate(panels):
    im = Image.open(path).convert("RGB")
    tile = im.crop((TOPLEFT[0], TOPLEFT[1], TOPLEFT[0] + CROP, TOPLEFT[1] + CROP))
    tile = tile.resize((w, w), Image.NEAREST)
    x = i * (w + 8)
    canvas.paste(tile, (x, 34))
    draw.text((x + 8, 12), " " + label + " ", fill=(235, 235, 235))

canvas.save(out)
print(f"wrote {out} ({canvas.width}x{canvas.height})")
