#!/usr/bin/env python3
"""Compare two PROBE_MESH_MASK dumps layer by layer, on the axis that matters to the artist.

The content level (the alpha test that decides which mesh cells exist) changes GEOMETRY, not
pixels: a layer's texture is untouched, so a render can hide the whole effect behind an opaque
layer, and a triangle count only says how dense the mesh is. What an artist sees when the cutoff
is too aggressive is the mesh silhouette retreating from the form they drew - a boundary losing
its last row, a soft edge turning into a stair step.

So this reads the per-layer mesh silhouettes that `PROBE_MESH_MASK=<dir>` writes (every triangle
of the mesh, filled) and reports, per layer:

  area      silhouette pixels lost / gained
  edge      how far the boundary moved (the largest inward retreat along the 4 sides)
  bbox      whether the silhouette's bounding box shrank
  rows      canvas rows where the silhouette is missing pixels, with their x runs

usage: mesh_silhouette_diff.py <dirA> <dirB> [--label-a A] [--label-b B] [--top N] [--tsv FILE]
       [--crop X Y W H] [--png DIR]

The mask PNGs are written in the *layer* frame (AE's dilated image frame: PSD rect shifted by
-1,-1), so a crop has to be given in that frame; `--png` writes a side-by-side crop per differing
layer with the differing pixels marked.
"""
import argparse
import os
import sys

import numpy as np


def load(path):
    from PIL import Image

    im = Image.open(path).convert("RGBA")
    return np.array(im)[:, :, 3] > 0


def runs(mask):
    """x runs of covered pixels per row: {row: [(x0, x1), ...]} (x1 exclusive)."""
    out = {}
    for y in np.nonzero(mask.any(axis=1))[0]:
        row = mask[y]
        idx = np.nonzero(row)[0]
        if idx.size == 0:
            continue
        splits = np.nonzero(np.diff(idx) > 1)[0]
        starts = np.concatenate(([idx[0]], idx[splits + 1]))
        ends = np.concatenate((idx[splits], [idx[-1]])) + 1
        out[int(y)] = [(int(a), int(b)) for a, b in zip(starts, ends)]
    return out


def edge_shift(a, b):
    """Per side: how far the boundary moved, signed so that POSITIVE = inward (b retreated).

    Scanning line by line along the side (rows for left/right, columns for top/bottom); lines
    covered in only one of the two silhouettes are skipped so a reshaped end cannot masquerade
    as an edge retreat.
    """
    res = {}
    sides = (
        ("left", 0, "first", +1),
        ("right", 0, "last", -1),
        ("top", 1, "first", +1),
        ("bottom", 1, "last", -1),
    )
    for name, axis, which, sign in sides:
        shifts = []
        lines = a.shape[0] if axis == 0 else a.shape[1]
        for line in range(lines):
            av = a[line, :] if axis == 0 else a[:, line]
            bv = b[line, :] if axis == 0 else b[:, line]
            ai = np.nonzero(av)[0]
            bi = np.nonzero(bv)[0]
            if ai.size == 0 or bi.size == 0:
                continue
            apos = ai[0] if which == "first" else ai[-1]
            bpos = bi[0] if which == "first" else bi[-1]
            shifts.append((bpos - apos) * sign)
        res[name] = (int(min(shifts)), int(max(shifts))) if shifts else (0, 0)
    return res


def bbox(mask):
    ys, xs = np.nonzero(mask)
    if ys.size == 0:
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir_a")
    ap.add_argument("dir_b")
    ap.add_argument("--label-a", default=None)
    ap.add_argument("--label-b", default=None)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--tsv", default=None)
    ap.add_argument("--crop", type=int, nargs=4, default=None, metavar=("X", "Y", "W", "H"))
    ap.add_argument("--png", default=None, help="write a per-layer side-by-side crop here")
    args = ap.parse_args()

    la = args.label_a or os.path.basename(args.dir_a.rstrip("/"))
    lb = args.label_b or os.path.basename(args.dir_b.rstrip("/"))
    names = sorted(set(os.listdir(args.dir_a)) & set(os.listdir(args.dir_b)))
    if not names:
        print("no common mask files in %s and %s" % (args.dir_a, args.dir_b))
        return 1

    rows = []
    only_a = sorted(set(os.listdir(args.dir_a)) - set(os.listdir(args.dir_b)))
    only_b = sorted(set(os.listdir(args.dir_b)) - set(os.listdir(args.dir_a)))
    for n in names:
        if not n.endswith(".png"):
            continue
        a = load(os.path.join(args.dir_a, n))
        b = load(os.path.join(args.dir_b, n))
        if a.shape != b.shape:
            rows.append((n, None, None, None, None, "size mismatch %s vs %s" % (a.shape, b.shape)))
            continue
        lost = int(np.count_nonzero(a & ~b))
        gained = int(np.count_nonzero(b & ~a))
        if lost == 0 and gained == 0:
            continue
        rows.append((n, lost, gained, edge_shift(a, b), bbox(a), bbox(b)))
    rows.sort(key=lambda r: -(r[1] or 0) - (r[2] or 0))

    print("=== mesh silhouettes: %s vs %s ===" % (la, lb))
    print("layers compared: %d   layers whose silhouette differs: %d" % (len(names), len(rows)))
    if only_a:
        print("only in %s: %d (%s)" % (la, len(only_a), ", ".join(only_a[:5])))
    if only_b:
        print("only in %s: %d (%s)" % (lb, len(only_b), ", ".join(only_b[:5])))
    if not rows:
        print("identical silhouettes everywhere")
        return 0
    print("\n%-26s %10s %10s   %-30s %s" % ("layer", "lost px", "gained px", "edge move L R T B", "bbox"))
    for n, lost, gained, es, ba, bb in rows[: args.top]:
        if es is None:
            print("%-26s %10s %10s   %s" % (n, "-", "-", bb))
            continue
        print("%-26s %10d %10d   L%+4d R%+4d T%+4d B%+4d   %s -> %s"
              % (n, lost, gained, es["left"][0], es["right"][0], es["top"][0], es["bottom"][0], ba, bb))
    tot_lost = sum(r[1] or 0 for r in rows)
    tot_gain = sum(r[2] or 0 for r in rows)
    print("\ntotal silhouette pixels lost by %s: %d   gained: %d   (over %d layers)"
          % (lb, tot_lost, tot_gain, len(rows)))

    if args.tsv:
        with open(args.tsv, "w") as fh:
            fh.write("layer\tlost_px\tgained_px\tleft\tright\ttop\tbottom\tbbox_a\tbbox_b\n")
            for n, lost, gained, es, ba, bb in rows:
                if es is None:
                    fh.write("%s\t\t\t\t\t\t\t%s\n" % (n, bb))
                    continue
                fh.write("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n"
                         % (n, lost, gained, es["left"][0], es["right"][0], es["top"][0],
                            es["bottom"][0], ba, bb))
        print("wrote %s" % args.tsv)

    if args.png and args.crop:
        from PIL import Image, ImageDraw

        x, y, w, h = args.crop
        os.makedirs(args.png, exist_ok=True)
        for n, lost, gained, es, ba, bb in rows[: args.top]:
            a = load(os.path.join(args.dir_a, n))
            b = load(os.path.join(args.dir_b, n))
            if a.shape != b.shape:
                continue
            only = a[y:y + h, x:x + w] & ~b[y:y + h, x:x + w]
            if not only.any():
                continue
            panels = []
            for m in (a, b):
                p = np.zeros((h, w, 3), np.uint8)
                sub = m[y:y + h, x:x + w]
                p[sub] = (200, 200, 200)
                p[only] = (255, 40, 40)
                panels.append(p)
            im = Image.new("RGB", (w * 2 + 8, h), (16, 16, 16))
            im.paste(Image.fromarray(panels[0]), (0, 0))
            im.paste(Image.fromarray(panels[1]), (w + 8, 0))
            im = im.resize(((w * 2 + 8) * 4, h * 4), Image.NEAREST)
            d = ImageDraw.Draw(im)
            d.text((4, 4), "%s   left=%s  right=%s" % (n, la, lb), fill=(255, 255, 0))
            d.text((4, 4 + (w * 2 + 8) * 2), "red = in %s, missing in %s  (%d px)" % (la, lb, int(only.sum())),
                   fill=(255, 160, 160))
            im.save(os.path.join(args.png, "%s_silhouette_diff.png" % n.replace(".png", "")))
        print("wrote silhouette crops to %s" % args.png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
