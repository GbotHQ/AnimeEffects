#!/usr/bin/env python3
"""Compare two output trees of cutoff_batch.sh - i.e. two revisions of the MESH code - per file.

Two renders of one file that were produced by the same engine differ only by the cutoff, so the
question "what did the fix change" needs both engines on the same file and the same variant. This
reads `<treeA>/<file>/<variant>/render_straight.png` and the same path in tree B, and reports

  * px changed / of which >8: what the engine change did to that render (max channel difference),
  * and, where the PSD's own stored composite is available in the tree, each engine's mean absolute
    difference against it - the artwork's own ground truth for the pixels the mesh refused to draw.

The composite is the artist's render of the file as they authored it, so it can say which engine is
closer for COVERAGE defects. It cannot arbitrate the content level itself (the composite contains
whatever the level is meant to decide), so the table is evidence about the engine, not about T.

usage: engine_diff.py <treeA> <treeB> [--label-a A] [--label-b B] [--variants const10,...] [--tsv F]
"""
import argparse
import os
import sys

import numpy as np


def load(path):
    from PIL import Image

    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)


# the same variant can be spelled differently in an older tree
ALIAS = {"off": "const10", "none": "nocut"}


def find(tree, name, variant):
    for v in (variant, *(k for k, val in ALIAS.items() if val == variant)):
        p = os.path.join(tree, name, v, "render_straight.png")
        if os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree_a")
    ap.add_argument("tree_b")
    ap.add_argument("--label-a", default=None)
    ap.add_argument("--label-b", default=None)
    ap.add_argument("--variants", default="const10,nocut")
    ap.add_argument("--tsv", default=None)
    args = ap.parse_args()

    la = args.label_a or os.path.basename(args.tree_a.rstrip("/"))
    lb = args.label_b or os.path.basename(args.tree_b.rstrip("/"))
    variants = [v.strip() for v in args.variants.split(",") if v.strip()]

    files = sorted(d for d in os.listdir(args.tree_a) if os.path.isdir(os.path.join(args.tree_a, d)))
    print("=== engine comparison: %s vs %s ===" % (la, lb))
    print("%-9s %-8s %12s %10s   %s" % ("file", "variant", "px changed", "of >8", "mean |diff| vs the stored composite"))
    tot = {}
    rows = []
    for name in files:
        for v in variants:
            pa, pb = find(args.tree_a, name, v), find(args.tree_b, name, v)
            if pa is None or pb is None:
                continue
            a, b = load(pa), load(pb)
            if a.shape != b.shape:
                print("%-9s %-8s skipped: %s vs %s" % (name, v, a.shape, b.shape))
                continue
            d = np.abs(a - b).max(axis=2)
            line = (name, v, int((d > 0).sum()), int((d > 8).sum()))
            # each engine's mean |diff| against the file's stored composite, when the tree has it
            vals = []
            for tree, img in ((args.tree_a, a), (args.tree_b, b)):
                c = os.path.join(tree, "composite_%s.png" % name)
                vals.append(np.abs(load(c) - img).max(axis=2).mean()
                            if os.path.exists(c) else float("nan"))
            detail = "%8.4f (%s)   %8.4f (%s)" % (vals[0], la, vals[1], lb)
            print("%-9s %-8s %12d %10d   %s" % (line[0], line[1], line[2], line[3], detail))
            rows.append(line + (detail,))
            k = v
            t = tot.setdefault(k, [0, 0])
            t[0] += line[2]
            t[1] += line[3]
    print("\n%-8s %12s %10s" % ("variant", "px changed", "of >8"))
    for v, (c, o) in sorted(tot.items()):
        print("%-8s %12d %10d" % (v, c, o))
    if args.tsv:
        with open(args.tsv, "w") as fh:
            fh.write("file\tvariant\tpx_changed\tover8\tcomposite_detail\n")
            for r in rows:
                fh.write("%s\t%s\t%d\t%d\t%s\n" % r)
        print("wrote %s" % args.tsv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
