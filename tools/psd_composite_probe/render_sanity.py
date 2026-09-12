#!/usr/bin/env python3
"""Check a cutoff_batch.sh tree for renders that the GPU got wrong.

Every variant of a file is rendered by its own process, and the only difference between two variants
is the mesh, so a pixel that is far from the PSD's own stored composite AND far from every sibling
variant is not explainable by any cutoff: it is a rendering fault. Measured on a 30-file corpus this
found sparse saturated speckles (black/cyan/magenta dots on skin tones) in three renders, max 250
levels wrong; re-rendering the same file and variant twice produced byte-identical output, so the
code is deterministic and the fault was in the GPU - it happens on this machine under a long batch.

usage: render_sanity.py <batchDir> [--block N] [--min-px N] [--diff N] [--files F ...]

Exit status is 1 when any render is flagged, so a batch can gate on it.
"""
import argparse
import os
import sys

import numpy as np


def load(path):
    from PIL import Image

    try:
        return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)
    except (OSError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("batch")
    ap.add_argument("--block", type=int, default=32, help="block size for locating a cluster")
    ap.add_argument("--min-px", type=int, default=50, help="inconsolable pixels needed to flag")
    ap.add_argument("--diff", type=int, default=100, help="level difference that counts as far")
    ap.add_argument("--files", nargs="*", default=None)
    args = ap.parse_args()

    names = args.files or sorted(d for d in os.listdir(args.batch)
                                 if os.path.isdir(os.path.join(args.batch, d)))
    flagged = 0
    checked = 0
    for name in names:
        d = os.path.join(args.batch, name)
        comp = load(os.path.join(args.batch, "composite_%s.png" % name))
        variants = {}
        for v in sorted(os.listdir(d)):
            img = load(os.path.join(d, v, "render_straight.png"))
            if img is not None:
                variants[v] = img
        if comp is None or len(variants) < 2:
            continue
        for v, a in variants.items():
            checked += 1
            others = [b for w, b in variants.items() if w != v and b.shape == a.shape]
            if not others or comp.shape != a.shape:
                continue
            mask = np.abs(a - comp).max(axis=2) > args.diff
            for b in others:
                mask &= np.abs(a - b).max(axis=2) > args.diff
            total = int(mask.sum())
            if total < args.min_px:
                continue
            worst = 0
            at = None
            for y in range(0, mask.shape[0], args.block):
                for x in range(0, mask.shape[1], args.block):
                    c = int(mask[y:y + args.block, x:x + args.block].sum())
                    if c > worst:
                        worst = c
                        at = (x, y)
            flagged += 1
            print("%-10s %-10s %7d pixels disagree with the composite AND every sibling  "
                  "worst %dx%d block %d px at %s" % (name, v, total, args.block, args.block, worst, at))
    print("\nchecked %d renders, flagged %d" % (checked, flagged))
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
