"""Builds a browsable index of a cutoff batch, in a durable directory, for every file whose
renders are complete. Safe to re-run while a batch is still running: it only reads finished files
and regenerates the index.

usage: cutoff_site.py <batchDir> [<siteDir>]

Each file gets a row with the numbers (pixels changed against the cutoff-off render, and how many
moved by more than 8 levels) and links to the three renders - off (= the constant T=10), mean,
ratio - plus the PSD's own stored composite and the largest changed-region crops, which are the
1:1 before/after/difference strips.
"""
import html
import json
import os
import shutil
import sys
import numpy as np
from PIL import Image


def copy_if_newer(src, dst):
    """Copy only when needed: the rebuild runs every few seconds while a batch is live, and the
    renders are large PNGs, so an unconditional copy would be pure IO."""
    try:
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            return
        shutil.copyfile(src, dst)
    except OSError:
        pass

# The baseline directory used to be called "off", which reads as "no cutoff" - it is the opposite:
# it is the CONSTANT T=10 test, i.e. what ships today. It is called const10 now (and nocut is the
# no-cutoff run), so nothing has to be inferred from a directory name; the old spellings are still
# accepted so an output tree from an earlier pass keeps rendering.
DIRNAME = {'off': 'const10', 'none': 'nocut'}
BASELINE_NAMES = ('const10', 'off')
NOCUT_NAMES = ('nocut', 'none')


def dirlabel(v):
    return DIRNAME.get(v, v)


def human(v):
    v = dirlabel(v)
    if v == 'const10':
        return 'const10 (the fixed T=10 level, what shipped before)'
    if v == 'nocut':
        return 'nocut (no cutoff at all: alpha != 0)'
    if v == 'mean':
        return 'mean rule - NOW PRODUCTION (T = 0.05 x mean of alpha>2)'
    if v.startswith('ratio'):
        return 'ratio rule (anchor %s)' % v[5:]
    return v


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.int16)


def build(batch, site):
    return _build(os.path.abspath(batch), os.path.abspath(site))


def _signature(paths):
    sig = []
    for p in paths:
        try:
            st = os.stat(p)
            sig.append([os.path.basename(os.path.dirname(p)), int(st.st_mtime), st.st_size])
        except OSError:
            sig.append([os.path.basename(os.path.dirname(p)), 0, 0])
    return sig


def _cache_read(site):
    try:
        with open(os.path.join(site, '_numbers.json')) as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def _cache_write(site, cache):
    try:
        with open(os.path.join(site, '_numbers.json'), 'w') as fh:
            json.dump(cache, fh)
    except OSError:
        pass


def _build(base, site):
    os.makedirs(site, exist_ok=True)
    cache = _cache_read(site)
    rows = []
    for name in sorted(os.listdir(base)):
        d = os.path.join(base, name)
        if not os.path.isdir(d):
            continue
        # the baseline is the production constant-T run, whatever this tree calls it
        baseDir = next((v for v in BASELINE_NAMES
                        if os.path.exists(os.path.join(d, v, 'render_straight.png'))), None)
        if baseDir is None:
            continue
        offPath = os.path.join(d, baseDir, 'render_straight.png')
        variants = [v for v in sorted(os.listdir(d))
                    if v != baseDir and os.path.exists(os.path.join(d, v, 'render_straight.png'))]
        out = os.path.join(site, name)
        os.makedirs(out, exist_ok=True)
        entries = []
        for v in [baseDir] + variants:
            src = os.path.join(d, v, 'render_straight.png')
            dst = os.path.join(out, 'render_%s.png' % dirlabel(v))
            copy_if_newer(src, dst)
            entries.append((v, 'render_%s.png' % dirlabel(v), None, None, None))
        comp = os.path.join(base, 'composite_%s.png' % name)
        if os.path.exists(comp):
            copy_if_newer(comp, os.path.join(out, 'composite.png'))
        # Two references, because they answer different questions: against const10 (the constant T=10
        # test that ships today) a row says what the rule CHANGES, and against nocut (no cutoff at
        # all) it says what the rule still REMOVES - which is the "there is still a visible cutoff"
        # question.
        refName = next((v for v in NOCUT_NAMES
                        if os.path.exists(os.path.join(d, v, 'render_straight.png'))), None)
        sig = _signature([offPath] + [os.path.join(d, v, 'render_straight.png') for v in variants]
                         + ([os.path.join(d, refName, 'render_straight.png')] if refName else []))
        hit = cache.get(name)
        if hit is not None and hit.get('sig') == sig:
            numbers = [tuple(n) for n in hit['numbers']]
        else:
            # decoding a 4175x2976 render takes ~0.7 s, and there are one baseline plus every
            # variant per file: without this cache a page rebuild took two minutes, so the numbers
            # are kept next to the page and only recomputed for a file whose renders changed
            off = load(offPath)
            ref = load(os.path.join(d, refName, 'render_straight.png')) if refName else None
            numbers = []
            for v in variants:
                var = load(os.path.join(d, v, 'render_straight.png'))
                if var.shape != off.shape:
                    continue
                do = np.abs(off - var).max(axis=2)
                dr = np.abs(ref - var).max(axis=2) if (ref is not None and ref.shape == var.shape) else None
                numbers.append((v, int((do > 0).sum()), int((do > 8).sum()), float(do.mean()),
                                int((dr > 0).sum()) if dr is not None else -1,
                                int((dr > 8).sum()) if dr is not None else -1))
            cache[name] = {'sig': sig, 'numbers': [list(n) for n in numbers]}
        # The crops are listed from the BATCH directory, which is the source of truth, and a crop
        # that is no longer there is deleted from the served copy. Listing them from the served
        # directory instead showed stale crops from an earlier engine revision next to the current
        # ones - the reader sees an old bug that is already fixed and cannot tell which is which.
        crops = sorted(c for c in os.listdir(d) if c.endswith('.png') and '_grid_tile' in c)
        for crop in crops:
            copy_if_newer(os.path.join(d, crop), os.path.join(out, crop))
        for stale in (c for c in os.listdir(out) if '_grid_tile' in c and c not in crops):
            try:
                os.remove(os.path.join(out, stale))
            except OSError:
                pass
        rows.append((name, numbers, crops))

    _cache_write(site, cache)

    with open(os.path.join(site, 'index.html'), 'w') as f:
        f.write('<html><head><meta charset="utf-8"><title>cutoff comparison</title>'
                '<meta http-equiv="refresh" content="20">'
                '<style>body{font:14px sans-serif;margin:24px;background:#181818;color:#ddd}'
                'a{color:#8cf}td,th{padding:4px 10px;border-bottom:1px solid #333;text-align:left}'
                'img{max-width:100%;border:1px solid #444;margin:8px 0}</style></head><body>')
        f.write('<h1>alpha cutoff comparison, real renders</h1>')
        f.write('<p><b>const10</b> is the old fixed level (T=10) and <b>mean</b> is what production '
                'now derives per layer - the two together are the before/after of that change. <b>nocut</b> is the '
                'same file with no cutoff at all (every nonzero alpha counts as content: the reference '
                'for "what does a cutoff remove"). <b>mean</b> and <b>ratio0.5x</b> derive a level per '
                'layer from that layer\'s own alpha statistics. Only the MESH changes - the pixels '
                'each layer draws are untouched - so two renders of a file differ by the cutoff '
                'alone, and a difference against the PSD\'s own composite cannot answer the '
                'question (that composite contains whatever the cutoff is meant to decide).</p>')
        f.write('<p>The ratio variant derives a level per layer as '
                '<b>T = clamp(round(10 x (1 - R/anchor)), 1, 10)</b> with '
                '<b>R = pixels with 2&lt;alpha&lt;=10 divided by pixels with alpha&gt;10</b> in that '
                'layer. The anchor is the R at which the layer counts as fully soft (T=1). So a '
                '<b>larger anchor means a HIGHER T</b>, i.e. stricter - it keeps LESS - and a smaller '
                'anchor keeps MORE faint content. T is rounded to a whole level and clamped, so a '
                'small anchor change often changes nothing at all, and this formula can only ever '
                'relax production (it can never exceed 10).</p>')
        f.write('<p>Each image below is ONE region of the canvas, shown for every rule, so the same '
                'region is compared by reading DOWN the image instead of hunting across three '
                'separately grouped strips. Rows: the reference <b>nocut</b> first, then <b>const10</b>, '
                'then each candidate rule. Columns: the rule\'s own rendering, its difference against '
                '<b>nocut</b> (what it still REMOVES - the "is there still a visible cutoff" '
                'question), and its difference against <b>const10</b> (what it CHANGES from what '
                'ships). Panels are 256x256 canvas pixels at 1:1 and differences are amplified 4x, so '
                'a grey wedge in a difference panel can still be a change of only 1-2 levels; the '
                'label says how many pixels in that tile really moved by more than 8 levels. Regions '
                'are the four where some rule differs most visibly from nocut, which is why a file '
                'whose whole-canvas numbers are large but faint can show few or no images.</p>')
        f.write('<p><b>The mean rule is the one that landed</b> (img/MeshContentLevel, committed): the '
                'level is derived per layer instead of being the constant 10, so the "mean" row of '
                'every file is now what the application does, and the "const10" row is the level it '
                'used to apply.</p>')
        f.write('<p>%d files complete. Regenerate with tools/psd_composite_probe/cutoff_site.py.</p>' % len(rows))
        f.write('<table><tr><th>file</th><th>rule</th>'
                '<th>vs const10: px changed</th><th>of which &gt;8</th>'
                '<th>vs nocut: px changed</th><th>of which &gt;8</th><th>renders</th></tr>')
        for name, numbers, crops in rows:
            first = True
            for v, changed, over8, mean, rchanged, rover8 in numbers:
                f.write('<tr><td>%s</td><td>%s</td><td>%d</td><td>%d</td><td>%s</td><td>%s</td><td>'
                        % (html.escape(name) if first else '', html.escape(human(v)), changed, over8,
                           rchanged if rchanged >= 0 else '-', rover8 if rover8 >= 0 else '-'))
                f.write('<a href="%s/render_const10.png">const10</a>' % name)
                f.write(' <a href="%s/render_%s.png">%s</a></td></tr>'
                        % (name, dirlabel(v), html.escape(dirlabel(v))))
                first = False
        f.write('</table>')
        for name, numbers, crops in rows:
            f.write('<h2>%s</h2>' % html.escape(name))
            if os.path.exists(os.path.join(site, name, 'composite.png')):
                f.write('<div>the PSD\'s own composite: <a href="%s/composite.png">composite.png</a></div>' % name)
            f.write('<div><a href="%s/render_const10.png">const10 (T=10, ships today)</a>' % name)
            for v, *_ in numbers:
                f.write(' | <a href="%s/render_%s.png">%s</a>'
                        % (name, dirlabel(v), html.escape(dirlabel(v))))
            f.write('</div>')
            if crops:
                f.write('<div style="color:#9c9">each image below is one region, and every row of it '
                        'is a different rule over that SAME region: reference first (nocut), then '
                        'const10, then the candidate rules. Compare down the image.</div>')
            for crop in crops:
                # crop = <file>_<variant>_tile<i>_x<x>_y<y>_<n>px_over8_<m>_max<k>.png
                    parts = crop[:-4].split('_')
                    label = crop
                    for k, piece in enumerate(parts):
                        if piece.startswith('tile') and k + 3 < len(parts):
                            label = 'tile %s at %s  -  %s' % (
                                piece[4:], ' '.join(parts[k + 1:k + 3]), parts[k + 3])
                            break
                    f.write('<div><span style="color:#888">%s</span><br>'
                            '<img src="%s/%s"></div>' % (html.escape(label), name, crop))
        f.write('</body></html>')
    print('site: %s/index.html  (%d files)' % (site, len(rows)))
    for name, numbers, crops in rows:
        print('   %-10s %s' % (name, '  '.join(
            '%s: %d px changed vs const10 (%d >8), %d vs nocut' % (v, c, o, rc)
            for v, c, o, _m, rc, _ro in numbers)))


def main():
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/ae_cutoff'))


if __name__ == '__main__':
    main()
