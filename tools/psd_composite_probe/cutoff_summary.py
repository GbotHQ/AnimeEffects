"""Joins the two halves of a cutoff batch: what the renders changed, and which layers caused it.

usage: cutoff_summary.py <batchDir> [<cutDir>]
  batchDir  the cutoff_batch.sh output (per file: off/ and one dir per variant, each with
            render_straight.png)
  cutDir    the per-layer tables (threshold_audit's "CUT" lines, one .cut file per PSD)
"""
import glob
import os
import sys
import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int16)


def layer_rows(cutDir):
    """file -> list of (layer, mean, R, T_mean, rm_mean, T_ratio, rm_ratio)"""
    out = {}
    for f in glob.glob(os.path.join(cutDir, '*.cut')):
        name = os.path.basename(f)[:-4]
        rows = []
        for line in open(f, errors='replace'):
            p = line.rstrip('\n').split('\t')
            if len(p) < 12:
                continue
            try:
                rows.append((p[1], float(p[2]), float(p[3]),
                             max(1, min(10, int(p[7]))), int(p[8]),
                             max(1, min(10, int(p[9]))), int(p[10])))
            except ValueError:
                continue
        out[name] = rows
    return out


def main():
    base = sys.argv[1]
    cutDir = sys.argv[2] if len(sys.argv) > 2 else '/tmp/cut30'
    layers = layer_rows(cutDir)
    print('%-9s %-13s %9s %7s %7s %8s   %s' % ('file', 'variant', 'changed', '%px', 'mean', '>8', 'largest driving layer'))
    worst = []
    for name in sorted(os.listdir(base)):
        d = os.path.join(base, name)
        offPath = os.path.join(d, 'off', 'render_straight.png')
        if not os.path.isdir(d) or not os.path.exists(offPath):
            continue
        off = load(offPath)
        for variant in sorted(os.listdir(d)):
            p = os.path.join(d, variant, 'render_straight.png')
            if variant == 'off' or not os.path.exists(p):
                continue
            var = load(p)
            if var.shape != off.shape:
                continue
            diff = np.abs(off - var).max(axis=2)
            changed = int((diff > 0).sum())
            rows = layers.get(name, [])
            if 'ratio' in variant:
                top = max(rows, key=lambda r: r[6], default=None)
                drv = 'T %d removes %d px' % (top[5], top[6]) if top else '-'
            else:
                top = max(rows, key=lambda r: r[4], default=None)
                drv = 'T %d removes %d px' % (top[3], top[4]) if top else '-'
            print('%-9s %-13s %9d %6.2f%% %7.3f %8d   %s' % (
                name, variant, changed, 100.0 * changed / diff.size, diff.mean(),
                int((diff > 8).sum()), drv))
            worst.append((changed + int((diff > 8).sum()) * 50, name, variant, changed, int((diff > 8).sum())))
    print('\nwhere to look first (weighted by changed pixels and pixels off by more than 8 levels):')
    for score, name, variant, changed, over8 in sorted(worst, reverse=True)[:10]:
        crops = sorted(glob.glob(os.path.join(base, name, '%s_%s_tile*.png' % (name, variant))))[:1]
        print('   %-9s %-13s %7d px changed, %6d over 8   %s' % (
            name, variant, changed, over8, crops[0] if crops else '(no crop)'))


if __name__ == '__main__':
    main()
