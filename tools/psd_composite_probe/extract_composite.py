"""Independent decoder for the PSD's section-5 merged (composite) image data.

Shares no code with AnimeEffects: parses the container with struct, decodes the
PackBits RLE itself, and writes the result as a PNG. Used as the ground truth for
"diff the render against the composite the PSD already contains".

usage: extract_composite.py [file.psd] [out.png]
  file.psd  PSD to decode (default: /home/gambot/Downloads/ALL FATHER F3 color fixes.psd)
  out.png   PNG to write  (default: ./composite.png)

Run this FIRST: psd_composite_probe diffs its render against the PNG this produces.
Requires python3 only (struct + zlib), no numpy/PIL.
"""
import struct
import sys
import zlib

DEFAULT_PSD = '/home/gambot/Downloads/ALL FATHER F3 color fixes.psd'

pos = [a for a in sys.argv[1:] if not a.startswith('-')]
if len(pos) > 2 or any(a in ('-h', '--help') for a in sys.argv[1:]):
    print(__doc__.strip())
    raise SystemExit(0 if any(a in ('-h', '--help') for a in sys.argv[1:]) else 2)
PSD = pos[0] if len(pos) > 0 else DEFAULT_PSD
OUT = pos[1] if len(pos) > 1 else 'composite.png'

f = open(PSD, 'rb')
sig, ver, res, ch, h, w, depth, mode = struct.unpack('>4sH6sHIIHH', f.read(26))
assert sig == b'8BPS', sig
print('input: %s' % PSD)
f.seek(struct.unpack('>I', f.read(4))[0], 1)   # color mode data
f.seek(struct.unpack('>I', f.read(4))[0], 1)   # image resources
lm = struct.unpack('>I', f.read(4))[0]
f.seek(lm, 1)                                  # layer and mask info
comp = struct.unpack('>H', f.read(2))[0]
print('composite: %dx%d channels=%d depth=%d mode=%d compression=%d' % (w, h, ch, depth, mode, comp))


def unpackbits(src, need):
    out = bytearray()
    i = 0
    n = len(src)
    while len(out) < need:
        if i >= n:
            raise ValueError('RLE data exhausted: got %d of %d bytes' % (len(out), need))
        b = src[i]
        i += 1
        if b < 128:
            cnt = b + 1
            out += src[i:i + cnt]
            i += cnt
        elif b > 128:
            cnt = 257 - b
            out += bytes([src[i]]) * cnt
            i += 1
        # b == 128 is a no-op
    return bytes(out[:need])


planes = []
if comp == 1:
    lens = [struct.unpack('>H', f.read(2))[0] for _ in range(ch * h)]
    print('scanline length range: %d..%d' % (min(lens), max(lens)))
    for c in range(ch):
        plane = bytearray()
        for y in range(h):
            plane += unpackbits(f.read(lens[c * h + y]), w)
        assert len(plane) == w * h, (c, len(plane))
        planes.append(bytes(plane))
elif comp == 0:
    planes = [f.read(w * h) for _ in range(ch)]
else:
    raise SystemExit('unsupported compression %d' % comp)

raw = bytearray()
for y in range(h):
    raw.append(0)  # PNG filter type 0
    for x in range(w):
        i = y * w + x
        raw += bytes((planes[0][i], planes[1][i], planes[2][i]))


def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)


ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
with open(OUT, 'wb') as o:
    o.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
print('wrote', OUT)
print('per-plane min/max:', [(min(p), max(p)) for p in planes])
