#!/usr/bin/env python3
"""One figure per artwork: the whole artwork, plus magnified crops as additional evidence.

Each figure shows ONE file, at both zooms that were measured:

    <file>  <canvas>  <layers> layers

    2x minification   mean/max error vs truth: level 0 a/b   mipmapped c/d   [better|same|worse]
    [ truth (supersampled) ]   [ level 0: what shipped ]   [ mipmapped: now ]   <- whole artwork;
                                                                                  red and blue boxes
                                                                                  mark the crops below
    [ truth ]   [ level 0 ]   [ mipmapped ]      <- crop in the RED box, x2 nearest
    [ truth ]   [ level 0 ]   [ mipmapped ]      <- crop in the BLUE box, x2 nearest
    ... the same three rows again at 4x ...

The whole-artwork row is context: it is downscaled to fit, so it can hide what is being judged. The
crop rows are the evidence: a 192-pixel window of the render, magnified x2 with nearest-neighbour
and never resampled, so every pixel shown is a rendered pixel. The red window is where level-0
sampling is furthest from the truth - the aliasing the change removes. The blue window is where the
mipmapped render is furthest from the truth - what the change costs. Both windows are found by
scanning the file's own difference maps, not chosen by hand.

Usage:
    make_artwork_figures.py <renderDir> <csv> <outDir> <file...>
"""
import csv
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

render_dir, csv_path, out_dir = sys.argv[1:4]
files = sys.argv[4:]

CROP, MAG, GAP = 128, 4, 14          # MAG is THE constant: every crop, every figure
PANELW = CROP * MAG                  # crop panel size, identical in every figure
X0 = 12
COPX = [X0 + i * (PANELW + GAP) for i in range(3)]
LINE, CAP = 22, 20
LABEL_H = 4 + 2 * LINE + CAP

try:
    FONT = ImageFont.truetype("/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", 15)
    FONT_B = ImageFont.truetype("/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", 19)
except Exception:  # fall back to the bitmap font rather than failing
    FONT = FONT_B = ImageFont.load_default()

BY_ZOOM = {}
for r in csv.DictReader(open(csv_path)):
    BY_ZOOM.setdefault(r["zoom"], {})[r["file"]] = r


def load(base, zoom_tag, kind):
    p = f"{render_dir}/{base}_z{zoom_tag}_{kind}.png"
    return np.asarray(Image.open(p).convert("RGB")) if os.path.exists(p) else None


def errs(ref, other):
    d = np.abs(ref.astype(np.int32) - other.astype(np.int32)).max(axis=2)
    return float(d.mean()), int(d.max())


def worst_window(a, b, size):
    d = np.abs(a.astype(np.int32) - b.astype(np.int32)).max(axis=2)
    h, w = d.shape
    size = min(size, h, w)
    step = max(1, size // 2)
    best, at = -1.0, (max(0, (w - size) // 2), max(0, (h - size) // 2))
    for y in range(0, h - size + 1, step):
        for x in range(0, w - size + 1, step):
            s = float(d[y : y + size, x : x + size].sum())
            if s > best:
                best, at = s, (x, y)
    return at


os.makedirs(out_dir, exist_ok=True)
written = []

for name in files:
    base = os.path.splitext(name)[0]
    zooms = []
    for zoom_tag, key in (("500", "0.5"), ("250", "0.25")):
        row = BY_ZOOM.get(key, {}).get(name)
        if not row:
            continue
        ref, before, after = (load(base, zoom_tag, k) for k in ("ref", "before", "after"))
        if ref is None or before is None or after is None:
            continue
        if not (ref.shape == before.shape == after.shape):
            continue
        zooms.append((row, ref, before, after))
    if not zooms:
        continue

    r0 = zooms[0][0]
    # The artwork panels are pasted at 1:1, so the figure's width follows the artwork: wide for a
    # large canvas, and never resampled to make it fit.
    width, height = 0, LABEL_H
    for row, ref, before, after in zooms:
        cw = int(round(int(row["canvas_w"]) * float(row["zoom"])))
        ch = int(round(int(row["canvas_h"]) * float(row["zoom"])))
        width = max(width, X0 + 3 * cw + 2 * GAP, X0 + 3 * PANELW + 2 * GAP)
        height += LINE + ch + CAP + 2 * (PANELW + CAP) + GAP
    fig = Image.new("RGB", (width, height + CAP), (18, 18, 18))
    draw = ImageDraw.Draw(fig)
    draw.text((X0, 8), f"{r0['file']}    {r0['canvas_w']}x{r0['canvas_h']}    "
                       f"{r0['layers']} layers", fill=(255, 214, 110), font=FONT_B)
    draw.text((X0, 8 + LINE), "columns:  supersampled ground truth   |   what shipped (level 0 minification)   |   "
                              f"now (mipmapped).   Artwork panels are the render at ACTUAL resolution, 1:1;   "
                              f"crops are {CROP} render pixels at x{MAG} nearest, the same factor in every figure",
              fill=(160, 160, 160), font=FONT)
    y = LABEL_H

    for row, ref, before, after in zooms:
        zoom = float(row["zoom"])
        cw = int(round(int(row["canvas_w"]) * zoom))
        ch = int(round(int(row["canvas_h"]) * zoom))
        ox, oy = (ref.shape[1] - cw) // 2, (ref.shape[0] - ch) // 2

        win_old = worst_window(before, ref, CROP)
        win_new = worst_window(after, ref, CROP)

        def clamp(at):
            return (min(max(at[0], ox), max(ox, ox + cw - CROP)),
                    min(max(at[1], oy), max(oy, oy + ch - CROP)))

        at_old, at_new = clamp(win_old), clamp(win_new)

        mb, xb = errs(ref, before)
        ma, xa = errs(ref, after)
        verdict = "same" if abs(ma - mb) < 0.02 else ("better" if ma < mb else "worse")
        colour = (190, 230, 190) if verdict == "better" else (
            (240, 175, 175) if verdict == "worse" else (205, 205, 205)
        )
        draw.text((X0, y), f"{int(round(1 / zoom))}x minification      mean/max error vs truth:  "
                           f"level 0  {mb:.2f} / {xb}      mipmapped  {ma:.2f} / {xa}      "
                           f"[{verdict} than level 0]" if verdict != "same" else "[no change vs level 0]", fill=colour, font=FONT)
        y += LINE

        # ACTUAL RESOLUTION: the render is pasted 1:1, never resampled, so the panel is exactly
        # what a screen pixel shows at this zoom and figures are comparable across files.
        for i, img in enumerate((ref, before, after)):
            tile = Image.fromarray(img[oy : oy + ch, ox : ox + cw])
            if i == 0:
                d = ImageDraw.Draw(tile)
                for at, col in ((at_old, (255, 70, 70)), (at_new, (80, 190, 255))):
                    d.rectangle([at[0] - ox, at[1] - oy, at[0] - ox + CROP, at[1] - oy + CROP],
                                outline=col, width=2)
            fig.paste(tile, (X0 + i * (cw + GAP), y))
        y += ch + CAP

        def colx(i, cw):
            return X0 + i * (cw + GAP) + (cw - PANELW) // 2

        for at, caption, col in (
            (at_old, f"RED box: {CROP}px of the render at x{MAG} nearest - where level 0 is furthest "
                     f"from the truth", (255, 100, 100)),
            (at_new, f"BLUE box: {CROP}px of the render at x{MAG} nearest - where mipmapped is "
                     f"furthest from the truth", (100, 190, 255)),
        ):
            for i, img in enumerate((ref, before, after)):
                tile = Image.fromarray(img[at[1] : at[1] + CROP, at[0] : at[0] + CROP])
                fig.paste(tile.resize((PANELW, PANELW), Image.NEAREST), (colx(i, cw), y))
            draw.text((X0, y + PANELW + 3), caption, fill=col, font=FONT)
            y += PANELW + CAP
        y += GAP

    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", base).strip("_") or "artwork"
    out = os.path.join(out_dir, safe + ".png")
    fig.save(out)
    written.append(out)

print(f"{len(written)} figures in {out_dir}")
