#!/usr/bin/env python3
"""One figure per artwork: the whole artwork, plus magnified crops as additional evidence.

Each figure shows ONE file, at each zoom that was measured:

    <file>  <canvas>  <layers> layers

    2x minification   mean/max error vs truth: level 0 a/b   mipmapped c/d   [better|same|worse]
    [ truth (supersampled) ]   [ level 0 only ]   [ mipmapped ]   [ level 4 forced ]   <- WHOLE artwork as
                                                                                  it appears at this
                                                                                  zoom, at 1:1; red
                                                                                  and blue boxes mark
                                                                                  the crops below
    [ truth ]   [ level 0 ]   [ mipmapped ]   [ level 4 ]   <- crop in the RED box, x4 nearest
    [ truth ]   [ level 0 ]   [ mipmapped ]   [ level 4 ]   <- crop in the BLUE box, x4 nearest
    ... the same three rows again at 4x, 8x and 16x ...

The fourth column is the same scene with level 4 FORCED (GL_TEXTURE_MIN_LOD = GL_TEXTURE_MAX_LOD =
4), which is the box pyramid's ideal output for a 16x footprint whatever the zoom would have chosen.
At 16x the shipped render reads level 4 anyway, so the third and fourth columns are then the same
pixels; at the other zooms forcing level 4 is visibly the wrong level, which is how the column is
known to be doing what it says. Read the two together and the question "is the mipmapped panel
blurrier than the truth because the sampling is wrong, or because a box filter is not the integral"
has a picture: where columns three and four agree, whatever is left is the filter.

The whole-artwork row is the render at ACTUAL resolution - one figure pixel is one render pixel,
never resampled, so it is exactly what a screen pixel shows at that zoom, and panels from different
figures are comparable. Its size follows the zoom: at 2x the artwork covers half the canvas, at 16x a
sixteenth of it, which is the whole point of the deep zooms. The crop rows are the evidence: a
128-pixel window of the render, magnified x4 with nearest-neighbour, the same factor in every figure
so two figures can be compared. The red window is where level-0 sampling is furthest from the truth -
the aliasing the change removes. The blue window is where the mipmapped render is furthest from the
truth - what the change costs. Both windows are found by scanning the file's own difference maps,
not chosen by hand.

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
X0 = 12
LINE, CAP = 22, 20
NCOL = 4                             # truth | level 0 | mipmapped | level 4 forced
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
    for zoom_tag, key in (("500", "0.5"), ("250", "0.25"), ("125", "0.125"), ("62", "0.0625")):
        row = BY_ZOOM.get(key, {}).get(name)
        if not row:
            continue
        ref, before, after, box4 = (load(base, zoom_tag, k) for k in ("ref", "before", "after", "box4"))
        if ref is None or before is None or after is None or box4 is None:
            continue
        if not (ref.shape == before.shape == after.shape == box4.shape):
            continue
        zooms.append((row, ref, before, after, box4))
    if not zooms:
        continue

    r0 = zooms[0][0]
    # The artwork panels are pasted at 1:1, so the figure's width follows the artwork: wide for a
    # large canvas, and never resampled to make it fit.
    width, height = 0, LABEL_H
    for row, ref, before, after, box4 in zooms:
        cw = ref.shape[1]
        ch = ref.shape[0]
        cr = min(CROP, cw, ch)
        width = max(width, X0 + NCOL * cw + (NCOL - 1) * GAP,
                    X0 + NCOL * (cr * MAG) + (NCOL - 1) * GAP)
        height += LINE + ch + CAP + 2 * (cr * MAG + CAP) + GAP
    fig = Image.new("RGB", (width, height + CAP), (18, 18, 18))
    draw = ImageDraw.Draw(fig)
    draw.text((X0, 8), f"{r0['file']}    {r0['canvas_w']}x{r0['canvas_h']}    "
                       f"{r0['layers']} layers", fill=(255, 214, 110), font=FONT_B)
    draw.text((X0, 8 + LINE), "columns:  supersampled ground truth   |   level 0 only (the behaviour before "
                              "this work)   |   mipmapped (what ships now)   |   level 4 forced (the box "
                              f"pyramid\'s ideal output).   Panels are the render at ACTUAL resolution, 1:1;   "
                              f"crops are at x{MAG} nearest, the same factor in every figure",
              fill=(160, 160, 160), font=FONT)
    y = LABEL_H

    for row, ref, before, after, box4 in zooms:
        zoom = float(row["zoom"])
        cw, ch = ref.shape[1], ref.shape[0]
        ox, oy = 0, 0

        win_old = worst_window(before, ref, CROP)
        win_new = worst_window(after, ref, CROP)

        def clamp(at):
            return (min(max(at[0], ox), max(ox, ox + cw - min(CROP, cw))),
                    min(max(at[1], oy), max(oy, oy + ch - min(CROP, ch))))

        at_old, at_new = clamp(win_old), clamp(win_new)

        mb, xb = errs(ref, before)
        ma, xa = errs(ref, after)
        m4, x4 = errs(ref, box4)
        impl, implmax = errs(after, box4)
        verdict = "same" if abs(ma - mb) < 0.02 else ("better" if ma < mb else "worse")
        colour = (190, 230, 190) if verdict == "better" else (
            (240, 175, 175) if verdict == "worse" else (205, 205, 205)
        )
        draw.text((X0, y), f"{int(round(1 / zoom))}x minification      mean/max vs truth:  level 0  "
                           f"{mb:.2f} / {xb}      mipmapped  {ma:.2f} / {xa}      level 4 forced  {m4:.2f} / {x4}"
                           f"      |  mipmapped vs level 4 forced (the sampling\'s own error):  {impl:.2f} / {implmax}"
                           f"      [{verdict} than level 0]" if verdict != "same"
                  else f"      [no change vs level 0]", fill=colour, font=FONT)
        y += LINE

        # ACTUAL RESOLUTION: the render is pasted 1:1, never resampled, so the panel is exactly
        # what a screen pixel shows at this zoom and figures are comparable across files.
        for i, img in enumerate((ref, before, after, box4)):
            tile = Image.fromarray(img[oy : oy + ch, ox : ox + cw])
            if i == 0:
                d = ImageDraw.Draw(tile)
                for at, col in ((at_old, (255, 70, 70)), (at_new, (80, 190, 255))):
                    d.rectangle([at[0] - ox, at[1] - oy, at[0] - ox + CROP, at[1] - oy + CROP],
                                outline=col, width=2)
            fig.paste(tile, (X0 + i * (cw + GAP), y))
        y += ch + CAP

        # Crops on their own grid, four columns wide, each panel exactly MAG x its window: with the
        # window following the panel, the MAGNIFICATION is the constant and the layout cannot
        # overlap - the earlier version centred the crops under panels narrower than a crop and the
        # three panels ran into each other at 8x and 16x.
        cr = min(CROP, cw, ch)
        cw2 = cr * MAG

        for at, caption, col in (
            (at_old, f"RED box: {cr}px of the render at x{MAG} nearest - where level 0 is furthest "
                     f"from the truth", (255, 100, 100)),
            (at_new, f"BLUE box: {cr}px of the render at x{MAG} nearest - where mipmapped is "
                     f"furthest from the truth", (100, 190, 255)),
        ):
            for i, img in enumerate((ref, before, after, box4)):
                tile = Image.fromarray(img[at[1] : at[1] + cr, at[0] : at[0] + cr])
                fig.paste(tile.resize((cw2, cw2), Image.NEAREST), (X0 + i * (cw2 + GAP), y))
            draw.text((X0, y + cw2 + 3), caption, fill=col, font=FONT)
            y += cw2 + CAP
        y += GAP

    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", base).strip("_") or "artwork"
    out = os.path.join(out_dir, safe + ".png")
    fig.save(out)
    written.append(out)

print(f"{len(written)} figures in {out_dir}")
