"""Fast metadata-only parse: per-layer flags and Photoshop 'Blend If' ranges.

Photoshop stores per-layer blending ranges (source/destination slider pairs) in the layer
record's extra data. A layer whose ranges are not the 0/0/255/255 defaults is restricted
by Blend If, which AnimeEffects' importer has no representation for at all.

usage: meta.py [file.psd]
  file.psd  PSD to parse (default: /home/gambot/Downloads/ALL FATHER F3 color fixes.psd)

Requires python3 only (struct + sys). Never reads the pixel data, so it is fast even on a
114 MB file.
"""
import struct
import sys

DEFAULT_PSD = '/home/gambot/Downloads/ALL FATHER F3 color fixes.psd'

if any(a in ('-h', '--help') for a in sys.argv[1:]):
    print(__doc__.strip())
    raise SystemExit(0)
PSD = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PSD
print('input: %s' % PSD)

fp = open(PSD, 'rb')
struct.unpack('>4sH6sHIIHH', fp.read(26))
fp.seek(struct.unpack('>I', fp.read(4))[0], 1)
fp.seek(struct.unpack('>I', fp.read(4))[0], 1)
fp.read(4)  # layer and mask info length
fp.read(4)  # layer info length
count = abs(struct.unpack('>h', fp.read(2))[0])

DEF = [(0, 0, 255, 255), (0, 0, 255, 255)]
rows = []
for idx in range(count):
    rect = struct.unpack('>4i', fp.read(16))
    top, left, bottom, right = rect
    nch = struct.unpack('>H', fp.read(2))[0]
    chans = [struct.unpack('>hI', fp.read(6)) for _ in range(nch)]
    fp.read(4)  # '8BIM'
    blend = fp.read(4).decode('latin-1')
    opacity, clipping, flags, filler = struct.unpack('>4B', fp.read(4))
    extra_len = struct.unpack('>I', fp.read(4))[0]
    extra_start = fp.tell()
    mask_len = struct.unpack('>I', fp.read(4))[0]
    mask = None
    if mask_len >= 20:
        mrect = struct.unpack('>4i', fp.read(16))
        mdef, mflags = struct.unpack('>2B', fp.read(2))
        mask = {'rect': mrect, 'default': mdef, 'flags': mflags}
        if mask_len >= 36:
            fp.read(2)
            mask['real_rect'] = struct.unpack('>4i', fp.read(16))
    fp.seek(extra_start + 4 + mask_len, 0)

    # layer blending ranges: composite pair + per-channel pairs
    blen = struct.unpack('>I', fp.read(4))[0]
    blobs = fp.read(blen)
    ranges = []
    for off in range(0, len(blobs), 8):
        src = tuple(blobs[off:off + 4])
        dst = tuple(blobs[off + 4:off + 8])
        ranges.append((src, dst))
    comp_range = ranges[0] if ranges else None
    chan_ranges = ranges[1:]

    name_len = fp.read(1)[0]
    fp.read(name_len)
    fp.read((4 - ((name_len + 1) % 4)) % 4)  # pascal name: len byte + text, padded to 4
    infos = {}
    while fp.tell() < extra_start + extra_len:
        sig = fp.read(4)
        if len(sig) < 4 or sig not in (b'8BIM', b'8B64'):
            break
        key = fp.read(4).decode('latin-1')
        ln = struct.unpack('>I', fp.read(4))[0]
        data = fp.read(ln)
        if ln % 2:
            fp.read(1)
        infos[key] = data
    fp.seek(extra_start + extra_len, 0)

    nondefault = [r for r in ([comp_range] + chan_ranges) if r and (r[0] != (0, 0, 255, 255) or r[1] != (0, 0, 255, 255))]
    rows.append(dict(idx=idx, name=infos.get('luni', b'').hex(), blend=blend, opa=opacity, flags=flags,
                     mask=mask, comp=comp_range, nchan_ranges=len(chan_ranges), nondefault=nondefault,
                     infos=infos))

names = {}
# names come from 'luni' (UTF-16BE, length-prefixed with the char count including NUL)
for r in rows:
    raw = bytes.fromhex(r['name'])
    if len(raw) >= 4:
        n = struct.unpack('>I', raw[:4])[0]
        txt = raw[4:4 + n * 2].decode('utf-16-be', 'replace').rstrip('\0')
        names[r['idx']] = txt
    else:
        names[r['idx']] = ''

print('idx  name                      blend opa  flags clip  mask  blend-if non-default   flags(infx/knko/clbl/lspf/iOpa/brst)')
for r in rows:
    fl = []
    for k in ('infx', 'knko', 'clbl', 'lspf'):
        if k in r['infos'] and len(r['infos'][k]) >= 1:
            fl.append('%s=%d' % (k, r['infos'][k][0]))
    if 'iOpa' in r['infos'] and len(r['infos']['iOpa']) == 4:
        fl.append('iOpa=%d' % r['infos']['iOpa'][0])
    if 'brst' in r['infos']:
        fl.append('brst=%d' % (r['infos']['brst'][0] if r['infos']['brst'] else -1))
    nd = ''
    if r['nondefault']:
        nd = 'YES ' + ' '.join('src%s/dst%s' % (str(x[0]), str(x[1])) for x in r['nondefault'][:3])
    print('%-4d %-25s %-5s %3d 0x%02x %4d  %-5s %-22s %s' % (
        r['idx'], names[r['idx']][:25], r['blend'], r['opa'], r['flags'], r['mask'] is not None,
        'masked' if r['mask'] else '', nd, ' '.join(fl)))

print()
print('layers with non-default blend-if ranges:', sum(1 for r in rows if r['nondefault']))
for r in rows:
    if r['nondefault']:
        print('   %-3d %-25s comp=%s channels=%d' % (r['idx'], names[r['idx']], r['comp'], r['nchan_ranges']))
