"""Independent PSD compositor (numpy) used to test whether the composite image stored in
the PSD is a faithful flatten of the layer stack, and to isolate what AnimeEffects' import
drops: layer masks, fill opacity (iOpa) and Photoshop 'Blend If' ranges.

Implements only what this file uses: Normal / Multiply / Overlay / Linear Burn /
Color Dodge / Color (PDF-spec non-separable) blends, straight-alpha 'over' compositing,
per-pixel weight = alpha * opacity * fillOpacity * mask * blend-if weight.
Pass-through groups are flattened inline (all three groups in this file are pass/255/100%).

usage: komp.py [file.psd] [out.png] [--no-blendif] [--no-mask] [--no-fillopa] [--no-effects]
               [--composite PNG] [--ae-render PNG]
  file.psd          PSD to composite (default: the 114 MB "ALL FATHER F3 color fixes.psd");
                    a first positional is taken as the PSD only when it ends in .psd/.psb
  out.png           PNG to write (default: ./komp_all.png)
  --no-blendif      ignore the layers' Photoshop blending ranges
  --no-mask         ignore layer masks (channel -2)
  --no-fillopa      ignore fill opacity (iOpa)
  --no-effects      accepted for symmetry; layer effects (lfx2) are never synthesized, so
                    this is a no-op
  --composite PNG   stored composite to compare against (default: ./composite.png)
  --ae-render PNG   AE render to compare against (default: ./render_straight.png, written
                    by psd_composite_probe into its output directory)

Requires python3 + numpy + PIL.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

DEFAULT_PSD = '/home/gambot/Downloads/ALL FATHER F3 color fixes.psd'
LUMA = np.array([0.3, 0.59, 0.11], dtype=np.float32)

args = sys.argv[1:]
if any(a in ('-h', '--help') for a in args):
    print(__doc__.strip())
    raise SystemExit(0)
opts = set(a for a in args if a.startswith('--'))
positional = []
i = 0
opt_value = {}
while i < len(args):
    if args[i] in ('--composite', '--ae-render'):
        opt_value[args[i]] = args[i + 1]
        i += 2
        continue
    if not args[i].startswith('--'):
        positional.append(args[i])
    i += 1

PSD = positional[0] if positional and positional[0].lower().endswith(('.psd', '.psb')) else DEFAULT_PSD
out_name = [a for a in positional if not a.lower().endswith(('.psd', '.psb'))]
OUT = out_name[0] if out_name else 'komp_all.png'
COMPOSITE = opt_value.get('--composite', 'composite.png')
AE_RENDER = opt_value.get('--ae-render', 'render_straight.png')
USE_BLENDIF = '--no-blendif' not in opts
USE_MASK = '--no-mask' not in opts
USE_FILLOPA = '--no-fillopa' not in opts

fnv_off = 0xcbf29ce484222325
fnv_prime = 0x100000001b3


def unpackbits(src, need):
    out = bytearray()
    i = 0
    n = len(src)
    ap = out.extend
    while len(out) < need:
        if i >= n:
            raise ValueError('RLE exhausted')
        b = src[i]
        i += 1
        if b < 128:
            ap(src[i:i + b + 1])
            i += b + 1
        elif b > 128:
            ap(bytes([src[i]]) * (257 - b))
            i += 1
    return bytes(out[:need])


# ---- blend functions (straight 0..1) -------------------------------------------------
def lum(c):
    return c @ LUMA


def clip_color(c):
    l = lum(c)
    mn = c.min(axis=-1)
    mx = c.max(axis=-1)
    out = c.copy()
    lo = mn < 0.0
    if lo.any():
        den = (l - mn)[lo]
        f = np.where(den != 0, l[lo] / np.where(den == 0, 1, den), 0.0)
        out[lo] = l[lo, None] + (c[lo] - l[lo, None]) * f[:, None]
    hi = mx > 1.0
    if hi.any():
        den = (mx - l)[hi]
        f = np.where(den != 0, (1.0 - l[hi]) / np.where(den == 0, 1, den), 0.0)
        out[hi] = l[hi, None] + (c[hi] - l[hi, None]) * f[:, None]
    return np.clip(out, 0.0, 1.0)


def set_lum(c, l):
    return clip_color(c + (l - lum(c))[..., None])


def blend(mode, cb, cs):
    if mode == 'norm':
        return cs
    if mode == 'mul ':
        return cb * cs
    if mode == 'over':
        return np.where(cb <= 0.5, 2.0 * cb * cs, 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs))
    if mode == 'lbrn':
        return np.clip(cb + cs - 1.0, 0.0, 1.0)
    if mode in ('div ', 'fdiv'):
        if mode == 'fdiv':
            return np.clip(cb / np.maximum(cs, 1e-6), 0.0, 1.0)
        return np.where(cb <= 0.0, 0.0, np.clip(cb / np.maximum(1.0 - cs, 1e-6), 0.0, 1.0))
    if mode == 'colr':
        return set_lum(cs, lum(cb))
    raise SystemExit('unhandled blend mode %r' % mode)


def blendif_weight(value, black, white):
    """Photoshop destination/source range weight (value in 0..255).

    The two edges of ONE slider combine with min(), not by multiplying: they agree on every
    ordinary range and differ only when the handles cross (blackHi > whiteLo), where the
    product under-weights the band. Measured on the 114 MB file, whose 7 layers with a
    non-default range include 3 genuinely crossed destination ranges: min() reproduces its
    stored composite at 0.8461 mean max-channel on effect-free pixels against 3.2933 for the
    product.
    """
    b0, b1 = black
    w0, w1 = white
    lo = np.ones_like(value, dtype=np.float32)
    hi = np.ones_like(value, dtype=np.float32)
    if b0 != 0 or b1 != 0:
        lo = np.clip((value - b0) / max(1e-6, b1 - b0), 0.0, 1.0)
    if not (w0 == 255 and w1 == 255):
        hi = np.clip((w1 - value) / max(1e-6, w1 - w0), 0.0, 1.0)
    return np.minimum(lo, hi)


def main():
    print('input: %s' % PSD)
    fp = open(PSD, 'rb')
    sig, ver, res, chans, H, W, depth, mode = struct.unpack('>4sH6sHIIHH', fp.read(26))
    assert sig == b'8BPS' and mode == 3 and depth == 8
    fp.seek(struct.unpack('>I', fp.read(4))[0], 1)
    fp.seek(struct.unpack('>I', fp.read(4))[0], 1)
    fp.read(4)
    fp.read(4)
    count = abs(struct.unpack('>h', fp.read(2))[0])

    records = []
    for idx in range(count):
        top, left, bottom, right = struct.unpack('>4i', fp.read(16))
        nch = struct.unpack('>H', fp.read(2))[0]
        ci = [struct.unpack('>hI', fp.read(6)) for _ in range(nch)]
        fp.read(4)
        bkey = fp.read(4).decode('latin-1')
        opacity, clipping, flags, _ = struct.unpack('>4B', fp.read(4))
        extra_len = struct.unpack('>I', fp.read(4))[0]
        extra_start = fp.tell()
        mask_len = struct.unpack('>I', fp.read(4))[0]
        mask = None
        if mask_len >= 20:
            mrect = struct.unpack('>4i', fp.read(16))
            mdef, mflags = struct.unpack('>2B', fp.read(2))
            mask = dict(rect=mrect, default=mdef, flags=mflags)
            if mask_len >= 36:
                fp.read(2)
                mask['real_rect'] = struct.unpack('>4i', fp.read(16))
        fp.seek(extra_start + 4 + mask_len, 0)
        blen = struct.unpack('>I', fp.read(4))[0]
        blobs = fp.read(blen)
        comp_range = None
        if len(blobs) >= 8:
            comp_range = (tuple(blobs[0:4]), tuple(blobs[4:8]))
        name_len = fp.read(1)[0]
        fp.read(name_len)
        fp.read((4 - ((name_len + 1) % 4)) % 4)  # pascal name: len byte + text, padded to 4
        infos = {}
        while fp.tell() < extra_start + extra_len:
            s = fp.read(4)
            if len(s) < 4 or s not in (b'8BIM', b'8B64'):
                break
            k = fp.read(4).decode('latin-1')
            ln = struct.unpack('>I', fp.read(4))[0]
            d = fp.read(ln)
            if ln % 2:
                fp.read(1)
            infos[k] = d
        fp.seek(extra_start + extra_len, 0)
        luni = infos.get('luni', b'')
        name = ''
        if len(luni) >= 4:
            n = struct.unpack('>I', luni[:4])[0]
            name = luni[4:4 + n * 2].decode('utf-16-be', 'replace').rstrip('\0')
        records.append(dict(rect=(top, left, bottom, right), ci=ci, blend=bkey, opa=opacity,
                            flags=flags, name=name, mask=mask, infos=infos, comp_range=comp_range))

    # ---- composite -------------------------------------------------------------------
    comp = np.zeros((H, W, 3), dtype=np.float32)   # backdrop RGB (straight)
    alpha = np.zeros((H, W), dtype=np.float32)     # backdrop alpha (stays 1 after layer 0)
    iopa_values = set()

    for L in records:
        top, left, bottom, right = L['rect']
        w, h = right - left, bottom - top
        visible = (L['flags'] & 0x02) == 0
        channel_data = {}
        for (cid, clen) in L['ci']:
            comp_id = struct.unpack('>H', fp.read(2))[0]
            payload = clen - 2
            pw, ph = w, h
            if cid == -2 and L['mask']:
                mr = L['mask'].get('real_rect') or L['mask']['rect']
                pw, ph = mr[3] - mr[1], mr[2] - mr[0]
            if comp_id == 0:
                data = fp.read(payload)[:pw * ph]
            else:
                lens = [struct.unpack('>H', fp.read(2))[0] for _ in range(ph)]
                buf = bytearray()
                for y in range(ph):
                    buf += unpackbits(fp.read(lens[y]), pw)
                data = bytes(buf)
            channel_data[cid] = (np.frombuffer(data, dtype=np.uint8).reshape(ph, pw), pw, ph)
        if not visible or w <= 0 or h <= 0 or L['blend'] in ('pass',):
            continue
        if not all(c in channel_data for c in (0, 1, 2)):
            continue

        src = np.stack([channel_data[c][0] for c in (0, 1, 2)], axis=-1).astype(np.float32) / 255.0
        a = (channel_data[-1][0].astype(np.float32) / 255.0) if -1 in channel_data else np.ones((h, w), np.float32)
        weight = a * (L['opa'] / 255.0)

        # fill opacity (iOpa): the spec stores a 4-byte value
        if 'iOpa' in L['infos'] and len(L['infos']['iOpa']) == 4:
            raw = L['infos']['iOpa']
            val = int.from_bytes(raw, 'little')
            if val > 255:
                val = raw[0]
            iopa_values.add(val)
            if USE_FILLOPA:
                weight = weight * (val / 255.0)

        # layer mask: aligned by canvas coordinates, default color outside the mask rect
        if USE_MASK and L['mask'] and -2 in channel_data:
            mp, pw, ph = channel_data[-2]
            if L['mask'].get('real_rect'):
                mleft, mtop = L['mask']['real_rect'][1], L['mask']['real_rect'][0]
            else:
                mleft, mtop = L['mask']['rect'][1], L['mask']['rect'][0]
            mw = np.full((h, w), float(L['mask']['default']), dtype=np.float32)
            x0, y0 = max(0, mleft - left), max(0, mtop - top)
            x1, y1 = min(w, mleft - left + pw), min(h, mtop - top + ph)
            if x1 > x0 and y1 > y0:
                mw[y0:y1, x0:x1] = mp[y0 - (mtop - top):y1 - (mtop - top), x0 - (mleft - left):x1 - (mleft - left)]
            weight = weight * (mw / 255.0)

        # clip the layer rect to the canvas (layers larger than the canvas are legal)
        sy0, sy1 = max(0, top), min(H, bottom)
        sx0, sx1 = max(0, left), min(W, right)
        if sy1 <= sy0 or sx1 <= sx0:
            continue
        oy0, oy1 = sy0 - top, sy1 - top
        ox0, ox1 = sx0 - left, sx1 - left
        src = src[oy0:oy1, ox0:ox1]
        weight = weight[oy0:oy1, ox0:ox1]

        # backdrop region for this layer
        cb = comp[sy0:sy1, sx0:sx1]
        ab = alpha[sy0:sy1, sx0:sx1]
        cb_straight = np.where(ab[..., None] > 0, cb / np.maximum(ab[..., None], 1e-6), 0.0)

        # Blend If: source range uses the layer's own luminance, destination the backdrop's
        if USE_BLENDIF and L['comp_range']:
            s_range, d_range = L['comp_range']
            if s_range != (0, 0, 255, 255):
                weight = weight * blendif_weight(lum(src) * 255.0, s_range[:2], s_range[2:])
            if d_range != (0, 0, 255, 255):
                weight = weight * blendif_weight(lum(cb_straight) * 255.0, d_range[:2], d_range[2:])

        b = blend(L['blend'], cb_straight, src)
        aw = weight[..., None]
        comp[sy0:sy1, sx0:sx1] = cb + aw * (b - cb)
        alpha[sy0:sy1, sx0:sx1] = ab + weight * (1.0 - ab)

    print('iOpa values seen:', sorted(iopa_values))
    img = np.clip(comp, 0, 1) * 255.0
    Image.fromarray(img.astype(np.uint8)).save(OUT)
    print('wrote', OUT)

    if not (os.path.exists(COMPOSITE) and os.path.exists(AE_RENDER)):
        print('skipping the comparison table: needs %s and %s (see --composite/--ae-render)' % (
            COMPOSITE, AE_RENDER))
        return
    ref = np.asarray(Image.open(COMPOSITE).convert('RGB')).astype(np.int16)
    ae = np.asarray(Image.open(AE_RENDER).convert('RGB')).astype(np.int16)
    mine = img.astype(np.int16)
    for name, other in (('independent compositor vs PSD composite', ref), ('AE render vs PSD composite', ref),
                        ('independent compositor vs AE render', ae)):
        d = np.abs((mine if name.startswith('independent compositor') else ae) - other).max(axis=2)
        print('%-40s mean=%7.3f max=%3d >2=%6.2f%% >8=%6.2f%% >32=%6.2f%%' % (
            name, d.mean(), d.max(), 100 * (d > 2).mean(), 100 * (d > 8).mean(), 100 * (d > 32).mean()))


main()
