#!/usr/bin/env python3
"""One figure per artwork: is minification correct at the alpha edges, where halos come from?

The failure this guards against is specific: averaging STRAIGHT colour across a transparency edge
mixes the RGB stored under alpha 0 into the visible edge, so a minified silhouette darkens (or
lightens) as it shrinks - the classic mipmap halo. The shipped chain premultiplies instead, and the
layer shader divides the coverage back out. The way to show that is not to describe it but to put
the render next to a supersampled ground truth at the edge itself, which is where a halo would be.

Per file, one figure at both measured zooms:

    <file>   <canvas>   <layers> layers
    columns: supersampled ground truth | what shipped (mipmapped) | level 0 minification (no chain)

    [ whole artwork, truth | shipped | level 0 ]
    [ magnified crop of the alpha edge furthest from the truth, x2 nearest ]
    error at partial-coverage pixels, where a halo would appear: ...

The crop window is found by scanning for the window that maximises the disagreement between the
shipped render and the truth AT PARTIAL-COVERAGE PIXELS ONLY - the place a halo would be. It is not
chosen by hand and it is not the window that flatters the change.

Usage:
    make_edge_figures.py <renderDir> <csv> <outDir> <file...>
"""
import csv
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

render_dir, csv_path, out_dir = sys.argv[1:4]
files = sys.argv[4:]

CROP, MAG, PANELW, GAP = 160, 2, 320, 14
X0 = 12
COLX = [X0 + i * (PANELW + GAP) for i in range(3)]
WIDTH = COLX[2] + PANELW + X0
LINE, CAP = 22, 20
LABEL_H = 8 + 2 * LINE + CAP

try:
    FONT = ImageFont.truetype("/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", 15)
    FONT_B = ImageFont.truetype("/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf", 19)
except Exception:
    FONT = FONT_B = ImageFont.load_default()

BY_ZOOM = {}
for r in csv.DictReader(open(csv_path)):
    BY_ZOOM.setdefault(r["zoom"], {})[r["file"]] = r


def load(base, zoom_tag, kind):
    p = f"{render_dir}/{base}_z{zoom_tag}_{kind}.png"
    return np.asarray(Image.open(p).convert("RGBA")) if os.path.exists(p) else None


def edge_window(shipped, truth, size):
    """The window where the shipped render is furthest from the truth at PARTIAL-COVERAGE pixels."""
    alpha = truth[:, :, 3]
    partial = ((alpha > 8) & (alpha < 247)).astype(np.float32)
    d = np.abs(shipped[:, :, :3].astype(np.int32) - truth[:, :, :3].astype(np.int32)).max(axis=2)
    score = d.astype(np.float32) * partial
    h, w = score.shape
    size = min(size, h, w)
    step = max(1, size // 2)
    best, at = -1.0, ((w - size) // 2, (h - size) // 2)
    for y in range(0, h - size + 1, step):
        for x in range(0, w - size + 1, step):
            s = float(score[y : y + size, x : x + size].sum())
            if s > best:
                best, at = s, (x, y)
    return at, partial


def edge_stats(shipped, truth, partial):
    d = np.abs(shipped[:, :, :3].astype(np.int32) - truth[:, :, :3].astype(np.int32)).max(axis=2)
    m = partial > 0
    n = int(m.sum())
    if n == 0:
        return 0.0, 0, 0
    return float(d[m].mean()), int(d[m].max()), n


os.makedirs(out_dir, exist_ok=True)
written = 0
skipped = []

for name in files:
    base = os.path.splitext(name)[0]
    zooms = []
    for zoom_tag, key in (("500", "0.5"), ("250", "0.25")):
        row = BY_ZOOM.get(key, {}).get(name)
        if not row:
            continue
        truth, shipped, level0 = (load(base, zoom_tag, k) for k in ("ref", "after", "before"))
        if truth is None or shipped is None or level0 is None:
            continue
        if not (truth.shape == shipped.shape == level0.shape):
            continue
        at, partial = edge_window(shipped, truth, CROP)
        if int(partial.sum()) < 200:          # no meaningful transparency: nothing to demonstrate
            continue
        zooms.append((row, truth, shipped, level0, at, partial))
    if not zooms:
        skipped.append(name)
        continue

    r0 = zooms[0][0]
    height = LABEL_H
    for row, truth, shipped, level0, at, partial in zooms:
        cw = int(round(int(row["canvas_w"]) * float(row["zoom"])))
        ch = int(round(int(row["canvas_h"]) * float(row["zoom"])))
        height += LINE + max(1, int(ch * min(PANELW / cw, PANELW / ch))) + CAP + PANELW + CAP + GAP
    fig = Image.new("RGB", (WIDTH, height + CAP), (18, 18, 18))
    draw = ImageDraw.Draw(fig)
    draw.text((X0, 8), f"{r0['file']}    {r0['canvas_w']}x{r0['canvas_h']}    {r0['layers']} layers",
              fill=(255, 214, 110), font=FONT_B)
    draw.text((X0, 8 + LINE), "columns:  supersampled ground truth   |   what shipped (mipmapped)   |   "
                              "level 0 minification, no chain", fill=(160, 160, 160), font=FONT)
    y = LABEL_H

    for row, truth, shipped, level0, at, partial in zooms:
        zoom = float(row["zoom"])
        cw = int(round(int(row["canvas_w"]) * zoom))
        ch = int(round(int(row["canvas_h"]) * zoom))
        ox, oy = (truth.shape[1] - cw) // 2, (truth.shape[0] - ch) // 2
        at = (min(max(at[0], ox), max(ox, ox + cw - CROP)), min(max(at[1], oy), max(oy, oy + ch - CROP)))

        se, sx, sn = edge_stats(shipped, truth, partial)
        le, lx, ln = edge_stats(level0, truth, partial)
        subs = "all" if sn >= 0 else ""
        draw.text((X0, y), f"{int(round(1 / zoom))}x minification    error vs truth at the "
                           f"{sn} partial-coverage pixels:   mipmapped {se:.2f} / {sx}"
                           f"      level 0 {le:.2f} / {lx}", fill=(200, 225, 200), font=FONT)
        y += LINE

        s = min(PANELW / cw, PANELW / ch)
        tw, th = max(1, int(cw * s)), max(1, int(ch * s))
        for i, img in enumerate((truth, shipped, level0)):
            tile = Image.fromarray(img[oy : oy + ch, ox : ox + cw, :3]).resize((tw, th), Image.LANCZOS)
            if i == 0:
                ImageDraw.Draw(tile).rectangle(
                    [round((at[0] - ox) * s), round((at[1] - oy) * s),
                     round((at[0] - ox + CROP) * s), round((at[1] - oy + CROP) * s)],
                    outline=(255, 70, 70), width=2
                )
            fig.paste(tile, (COLX[i] + (PANELW - tw) // 2, y))
        y += th + CAP

        for i, img in enumerate((truth, shipped, level0)):
            tile = Image.fromarray(img[at[1] : at[1] + CROP, at[0] : at[0] + CROP, :3])
            fig.paste(tile.resize((PANELW, PANELW), Image.NEAREST), (COLX[i], y))
        draw.text((X0, y + PANELW + 3),
                  f"the alpha edge the crop is centred on: {CROP}px of the render at x{MAG} nearest, "
                  f"never resampled - a halo would show here as the silhouette darkening or "
                  f"lightening against the truth", fill=(255, 100, 100), font=FONT)
        y += PANELW + CAP + GAP

    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", base).strip("_") or "artwork"
    fig.save(os.path.join(out_dir, safe + ".png"))
    written += 1

print(f"{written} edge figures written to {out_dir}")
if skipped:
    print(f"  skipped (no transparency to show): {len(skipped)} files")
