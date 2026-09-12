"""Independent Photoshop/PDF-spec reference for the Color blend, applied to the exact
inputs AE rendered (backdrop = AE's render of the stack below the layer; source = the
layer's own pixels as stored in the PSD).

PDF spec:
  Color(Cb, Cs) = SetLum(Cs, Lum(Cb))
  Lum(c)        = 0.3 R + 0.59 G + 0.11 B          (Rec.601, what PS/Krita/GIMP use)
  SetLum(c, l)  = ClipColor(c + (l - Lum(c)))
  ClipColor     = clamp at the ends while keeping Lum invariant
Compositing with source alpha As and opacity O:
  out = Cb + As' * (B(Cb, Cs) - Cb),  As' = As * O

usage: blend_ref.py [outDir] [composite.png] [--layer NAME]
  outDir          directory holding the probe's dumps (default: .). Needs
                  blend_backdrop.png and blend_upto_layer.png from the probe's blend probe
                  (PROBE_BLEND=1) and layer_<NAME>.png from PROBE_LAYER_DUMP=<NAME>.
  composite.png   stored composite to compare all three against (default: ./composite.png,
                  i.e. the output of extract_composite.py)
  --layer NAME    layer whose dump is the source (default: Layer 49). Spaces become '_' in
                  the dump filename.
Writes <outDir>/blend_reference.png (the independent prediction).

Requires python3 + numpy + PIL.
"""
import sys

import numpy as np
from PIL import Image


def lum(c):
    return c @ np.array([0.3, 0.59, 0.11])


def clip_color(c):
    l = lum(c)
    mn = c.min(axis=-1)
    mx = c.max(axis=-1)
    out = c.copy()
    # n < 0 branch
    lo = mn < 0.0
    if lo.any():
        denom = (l - mn)[lo]
        with np.errstate(divide='ignore', invalid='ignore'):
            f = np.where(denom != 0, l[lo] / denom, 0.0)
        out[lo] = l[lo, None] + (c[lo] - l[lo, None]) * f[:, None]
    hi = mx > 1.0
    if hi.any():
        denom = (mx - l)[hi]
        with np.errstate(divide='ignore', invalid='ignore'):
            f = np.where(denom != 0, (1.0 - l[hi]) / denom, 0.0)
        out[hi] = l[hi, None] + (c[hi] - l[hi, None]) * f[:, None]
    return np.clip(out, 0.0, 1.0)


def set_lum(c, l):
    return clip_color(c + (l - lum(c))[..., None])


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float64) / 255.0


args = sys.argv[1:]
if any(a in ('-h', '--help') for a in args):
    print(__doc__.strip())
    raise SystemExit(0)
layer = 'Layer 49'
out_dir = '.'
composite = 'composite.png'
positional = []
i = 0
while i < len(args):
    if args[i] == '--layer':
        layer = args[i + 1]
        i += 2
        continue
    if not args[i].startswith('--'):
        positional.append(args[i])
    i += 1
for a in positional:
    if a.lower().endswith('.png'):
        composite = a
    else:
        out_dir = a

backdrop = load('%s/blend_backdrop.png' % out_dir)
source_img = np.asarray(Image.open('%s/layer_%s.png' % (
    out_dir, layer.replace(' ', '_'))).convert('RGBA')).astype(np.float64) / 255.0
ae = load('%s/blend_upto_layer.png' % out_dir)
comp = load(composite)
print('layer', layer, 'backdrop', backdrop.shape, 'source', source_img.shape, 'AE', ae.shape)

src = source_img[..., :3]
alpha = source_img[..., 3:4]
expected = backdrop + alpha * (set_lum(src, lum(backdrop)) - backdrop)

d = np.abs(expected - ae).max(axis=2) * 255.0
print('AE vs independent Color-blend reference on identical inputs:')
print('   mean=%6.3f  max=%6.1f  >2=%6.3f%%  >8=%6.3f%%  >32=%6.3f%%' % (
    d.mean(), d.max(), 100 * (d > 2).mean(), 100 * (d > 8).mean(), 100 * (d > 32).mean()))

for name, img in (('AE', ae), ('independent ref', expected), ('PSD composite', comp)):
    dd = np.abs(img - comp).max(axis=2) * 255.0
    print('%-16s vs PSD composite: mean=%7.3f max=%6.1f' % (name, dd.mean(), dd.max()))

# save the prediction for eyeballing
Image.fromarray((np.clip(expected, 0, 1) * 255).astype(np.uint8)).save('%s/blend_reference.png' % out_dir)
