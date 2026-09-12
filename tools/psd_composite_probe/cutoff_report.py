"""Turns a cutoff batch into the artifacts to compare: real 1:1 crops, not alpha masks.

For every file it writes, per tile, ONE comparison grid so that the same region can be read down
the image instead of across three separately grouped strips:

    +----------+-------------------+----------------------+-----------------------+
    |          | this variant's    | difference x4        | difference x4         |
    |          | rendering         | vs nocut (keep all)  | vs const10 (today)    |
    +----------+-------------------+----------------------+-----------------------+
    | nocut    |  the reference: no cutoff at all (alpha != 0)
    | const10  |  what ships today: the constant T=10 test
    | mean     |  per-layer level from the layer's own mean alpha
    | ratio... |  per-layer level from the faint/strong pixel ratio
    +----------+-------------------+----------------------+-----------------------+

Every panel is 256x256 canvas pixels at 1:1, and every label is drawn into the image, so a grid
explains itself out of context. Tiles are ranked by the most VISIBLE difference (pixels moving more
than 8 levels) any variant makes against the reference, which is why a file whose whole-canvas
numbers are large but faint can still show few or no grids.

usage: cutoff_report.py <batchDir> [<name> ...]
"""
import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

TILE = 256          # the window a grid shows, in canvas pixels, at 1:1
TOP = 4             # how many tiles per file
LABW = 168          # the label column, in pixels
PAD = 4
ZOOM = 4            # the difference panels are amplified by this (the diff is what matters, and it
                    # is usually only a few levels)
VISIBLE = 8         # a change of more than this many levels is treated as visible


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int16)


def tiles(d, visible=VISIBLE):
    """Rank tiles by what can be SEEN in them, not by how many pixels moved.

    Ranking by the count of changed pixels picks tiles full of 1-level differences - thousands of
    them, none of them visible - over a tile with a handful of obvious ones, which is why the first
    crops mostly looked identical. Returns (score, x, y, w, h, visiblePixels, worstPixel).
    """
    h, w = d.shape
    out = []
    for y in range(0, h, TILE):
        for x in range(0, w, TILE):
            t = d[y:y + TILE, x:x + TILE]
            if not (t > 0).any():
                continue
            over = int((t > visible).sum())
            out.append((over * 1000000 + int(t.max()) * 1000 + int((t > 0).sum()),
                        x, y, t.shape[1], t.shape[0], over, int(t.max())))
    out.sort(reverse=True)
    return out


def _font(size=13):
    try:
        return ImageFont.load_default(size=size)
    except Exception:
        return ImageFont.load_default()


def variant_order(names):
    """The reference first, then what ships, then the candidate rules - the order the reader wants
    to compare in, not the alphabetical one."""
    order = []
    for fixed in ('nocut', 'none', 'const10', 'off'):
        if fixed in names:
            order.append(fixed)
    for n in sorted(names):
        if n not in order:
            order.append(n)
    return order


def build_grids(base, name, write=True):
    """Write one comparison grid per top tile for this file. Returns the per-variant whole-canvas
    numbers, keyed by (variant, reference)."""
    d = os.path.join(base, name)
    imgs = {}
    for v in sorted(os.listdir(d)):
        p = os.path.join(d, v, 'render_straight.png')
        if os.path.exists(p):
            imgs[v] = load(p)
    # the two references: the production constant-T run (const10, once called "off") and the
    # no-cutoff run (nocut, once "none")
    prod = 'const10' if 'const10' in imgs else ('off' if 'off' in imgs else None)
    ref = 'nocut' if 'nocut' in imgs else ('none' if 'none' in imgs else None)
    if prod is None and ref is None:
        return {}
    if prod is None:
        prod = ref
    if ref is None:
        ref = prod
    rows = [v for v in variant_order(list(imgs)) if v != ref]
    numbers = {}
    for v in rows:
        for r, other in ((ref, imgs[ref]), (prod, imgs[prod])):
            if v == r:
                continue
            diff = np.abs(other - imgs[v]).max(axis=2)
            numbers[(v, r)] = (int((diff > 0).sum()), int((diff > VISIBLE).sum()), float(diff.mean()))
    print('%-10s reference=%-5s %s' % (name, ref, '  '.join(
        '%s: %d px >8 vs %s' % (v, numbers.get((v, ref), (0, 0, 0))[1], ref) for v in rows)))
    if not write:
        return numbers

    diffRef = {}
    for v in rows:
        diffRef[v] = np.abs(imgs[ref] - imgs[v]).max(axis=2)
    # a tile counts as interesting if ANY variant visibly differs from the reference there
    mask = None
    for v in rows:
        m = (diffRef[v] > VISIBLE)
        mask = m if mask is None else (mask | m)
    if mask is None or not mask.any():
        return numbers
    ranked = []
    for y in range(0, mask.shape[0], TILE):
        for x in range(0, mask.shape[1], TILE):
            t = mask[y:y + TILE, x:x + TILE]
            if t.any():
                ranked.append((int(t.sum()), x, y, t.shape[1], t.shape[0]))
    ranked.sort(reverse=True)

    for i, (n, x, y, tw, th) in enumerate(ranked[:TOP]):
        crops = {}
        for v in [ref] + rows:
            crops[v] = imgs[v][y:y + th, x:x + tw].astype(np.uint8)
        diffs = {}
        for v in [ref] + rows:
            if v == ref:
                diffs[(v, ref)] = np.zeros((th, tw), np.uint8)
            else:
                diffs[(v, ref)] = np.clip(diffRef[v][y:y + th, x:x + tw] * ZOOM, 0, 255).astype(np.uint8)
            if v == prod:
                diffs[(v, prod)] = np.zeros((th, tw), np.uint8)
            else:
                dd = np.abs(imgs[prod] - imgs[v]).max(axis=2)
                diffs[(v, prod)] = np.clip(dd[y:y + th, x:x + tw] * ZOOM, 0, 255).astype(np.uint8)

        headerPx = 44
        W = LABW + PAD + tw + PAD + tw + PAD + tw
        # one row per rule, plus the reference row - a wrong count here does not error, PIL just
        # clips the extra rows away, which is how the ratio row went missing once
        H = headerPx + (1 + len(rows)) * th
        grid = np.full((H, W, 3), 24, np.uint8)
        im = Image.fromarray(grid)
        dr = ImageDraw.Draw(im)
        small = _font(12)
        refLabel = {'none': 'nocut', 'off': 'const10'}.get(ref, ref)
        prodLabel = {'none': 'nocut', 'off': 'const10'}.get(prod, prod)
        dr.text((6, 4), 'tile %d   canvas x=%d y=%d   %dx%d at 1:1   reference = %s%s'
                % (i, x, y, tw, th, refLabel,
                   ' (alpha != 0, no cutoff)' if refLabel == 'nocut' else ''),
                fill=(210, 230, 210), font=small)
        dr.text((6, 22), 'panels: [ the variant ] [ difference x%d vs %s ] [ difference x%d vs %s (T=10) ]'
                % (ZOOM, refLabel, ZOOM, prodLabel), fill=(150, 160, 150), font=small)
        colx = [LABW, LABW + tw + PAD, LABW + 2 * (tw + PAD)]
        for r, v in enumerate([ref] + rows):
            yy = headerPx + r * th
            label = {'none': 'nocut', 'off': 'const10'}.get(v, v)
            sub = ''
            if v != ref:
                c = numbers.get((v, ref), (0, 0, 0.0))
                sub = '%d px >%d vs nocut' % (c[1], VISIBLE)
            elif v == prod:
                sub = 'ships today (T=10)'
            else:
                sub = 'no cutoff at all'
            dr.text((6, yy + 6), label, fill=(230, 230, 230), font=_font(14))
            for k, line in enumerate(sub.split(' | ')):
                dr.text((6, yy + 26 + 14 * k), line, fill=(150, 170, 150), font=small)
            if v == ref:
                dr.text((6, yy + 40 + 14 * (1 + sub.count('|'))), 'reference row', fill=(120, 130, 120),
                        font=small)
            im.paste(Image.fromarray(crops[v]), (colx[0], yy))
            for k, refname in enumerate([ref, prod]):
                im.paste(Image.fromarray(diffs[(v, refname)]).convert('RGB'), (colx[k + 1], yy))
        # into the FILE's own directory (base/<name>/), which is where has_crops looks and what the
        # site copies from - not into the batch root
        out = os.path.join(d, '%s_grid_tile%d_x%d_y%d_%dpx_over%d.png'
                           % (name, i, x, y, n, VISIBLE))
        im.save(out)
    return numbers


def report(base, names=None, write_crops=True):
    """Kept for callers that only want the numbers (the server builds grids)."""
    names = names or sorted(d for d in os.listdir(base) if os.path.isdir(os.path.join(base, d)))
    rows = []
    for name in names:
        numbers = build_grids(base, name, write=write_crops)
        for (v, r), (changed, over, mean) in sorted(numbers.items()):
            rows.append(dict(file=name, variant=v, reference=r, changed=changed, over8=over, mean=mean))
    return rows


def has_crops(base, name):
    d = os.path.join(base, name)
    return any('_grid_tile' in f for f in os.listdir(d)) if os.path.isdir(d) else False


def main():
    base = sys.argv[1]
    names = sys.argv[2:] or None
    report(base, names, write_crops=True)
    return


if __name__ == '__main__':
    main()
