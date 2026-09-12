#!/usr/bin/env python3
"""Builds the corpus contact sheets: many artworks, each against supersampled ground truth.

Rows are files, columns are, left to right:

    GROUND TRUTH (supersampled)  |  BEFORE (level 0, what shipped)  |  AFTER (mipmapped, now)

The window shown for each file is not chosen by hand: it is the window where the two
implementations disagree most about the ground truth, found by scanning the file's own
difference maps on a grid. So a row that looks identical is a row where the change is invisible,
and the ones that differ are the ones where it matters.

Numbers on each row are mean max-channel error against the ground truth over the WHOLE canvas
(not just the crop), in 8-bit units, for the two implementations. "better/worse" is the AFTER
against the BEFORE.

Usage:
    make_corpus_sheet.py <renderDir> <csv> <zoomTag> <outPng> <title>
        renderDir : where --eval with AE_EVAL_SAVE=1 wrote <base>_z<zoomTag>_{ref,before,after}.png
        csv       : the --eval csv, for the per-file errors and the file order
        zoomTag   : 500 for 2x or 250 for 4x
"""
import csv
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

render_dir, csv_path, zoom_tag, out_path, title = sys.argv[1:6]
CROP, SCALE, GAP, HEADER = 30, 5, 6, 22


def errors(ref, a, b):
    d = lambda x, y: np.abs(x.astype(np.int32) - y.astype(np.int32)).max(axis=2)
    return d(a, ref).mean(), d(b, ref).mean()


def worst_window(pre, post, ref, size):
    """The window where the two implementations disagree most about the truth."""
    d = np.abs(pre.astype(np.int32) - post.astype(np.int32)).max(axis=2)
    h, w = d.shape
    size = min(size, h, w)
    best, at = -1.0, (0, 0)
    for y in range(0, h - size, max(1, size // 3)):
        for x in range(0, w - size, max(1, size // 3)):
            s = float(d[y : y + size, x : x + size].mean())
            if s > best:
                best, at = s, (x, y)
    return at


rows = [r for r in csv.DictReader(open(csv_path)) if abs(float(r["zoom"]) * 1000 - int(zoom_tag)) < 1]
rows.sort(key=lambda r: float(r["after_mean"]) - float(r["before_mean"]))

loaded = []
for r in rows:
    base = os.path.splitext(r["file"])[0]
    try:
        ref = np.asarray(Image.open(f"{render_dir}/{base}_z{zoom_tag}_ref.png").convert("RGB"))
        before = np.asarray(Image.open(f"{render_dir}/{base}_z{zoom_tag}_before.png").convert("RGB"))
        after = np.asarray(Image.open(f"{render_dir}/{base}_z{zoom_tag}_after.png").convert("RGB"))
    except FileNotFoundError:
        continue
    if ref.shape != before.shape or ref.shape != after.shape:
        continue
    loaded.append((r, ref, before, after))

if not loaded:
    sys.exit(f"no rendered panels found in {render_dir} for zoom {zoom_tag}")

panel = CROP * SCALE
width = panel * 3 + GAP * 2
height = HEADER + len(loaded) * (panel + HEADER)
sheet = Image.new("RGB", (width, height), (20, 20, 20))
draw = ImageDraw.Draw(sheet)

y = 0
for r, ref, before, after in loaded:
    x0, y0 = worst_window(before, after, ref, CROP)
    mb, ma = errors(ref, before, after)
    verdict = "same" if abs(ma - mb) < 0.02 else ("better" if ma < mb else "WORSE")
    draw.text((6, y + 6), f"{r['file']}  {r['canvas_w']}x{r['canvas_h']}  {r['layers']} layers  ss={r['ss']}"
                         f"   err vs ground truth: BEFORE(level 0) {mb:.2f} -> AFTER(mipmapped) {ma:.2f}"
                         f"  [{verdict}]", fill=(238, 238, 238))
    y += HEADER
    for i, img in enumerate((ref, before, after)):
        tile = Image.fromarray(img[y0 : y0 + CROP, x0 : x0 + CROP]).resize((panel, panel), Image.NEAREST)
        sheet.paste(tile, (i * (panel + GAP), y))
    y += panel

draw.text((6, 6), title, fill=(255, 220, 120))
sheet.save(out_path)
print(f"wrote {out_path} ({sheet.width}x{sheet.height}), {len(loaded)} files")
print(f"  better {sum(1 for r,_,_,_ in loaded if float(r['after_mean']) < float(r['before_mean']) - 0.02)},"
      f" same {sum(1 for r,_,_,_ in loaded if abs(float(r['after_mean']) - float(r['before_mean'])) <= 0.02)},"
      f" worse {sum(1 for r,_,_,_ in loaded if float(r['after_mean']) > float(r['before_mean']) + 0.02)}")
