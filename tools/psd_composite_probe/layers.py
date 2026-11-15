"""Independent PSD layer parser + channel decoder (shares no code with AnimeEffects).

Parses the layer records and the per-layer channel image data blocks, decodes raw and
PackBits RLE planes, and prints a per-layer FNV-1a checksum of the interleaved RGBA bytes
so it can be compared byte-for-byte against what img::PSDUtil::makeInterleavedImage
produces for the same layer (the probe prints `decode: ... fnv1a=...` per record).

usage: layers.py [file.psd] [layer name ...] [--out DIR]
  file.psd    PSD to parse (default: $AE_PROBE_PSD, which is required when no file is given);
              a first positional is taken as the PSD only when it ends in .psd/.psb, so
              layer names can also be given first.
  layer name  names whose decoded RGBA bytes are also dumped as raw files
  --out DIR   directory for those dumps (default: .), as py_layer_<name>.rgba

Requires python3 only (struct + sys).
"""
import struct
import os
import sys

DEFAULT_PSD = os.environ.get(
    'AE_PROBE_PSD', ''
)  # an absolute path to one person's file has no business being the default of a tool in
   # a repository: set AE_PROBE_PSD (or pass the file) and this stays reproducible anywhere

args = sys.argv[1:]
if any(a in ('-h', '--help') for a in args):
    print(__doc__.strip())
    raise SystemExit(0)
OUT_DIR = '.'
want_dump = set()
i = 0
while i < len(args):
    if args[i] == '--out':
        OUT_DIR = args[i + 1]
        i += 2
        continue
    if i == 0 and args[i].lower().endswith(('.psd', '.psb')):
        i += 1
        continue
    want_dump.add(args[i])
    i += 1
PSD = args[0] if args and args[0].lower().endswith(('.psd', '.psb')) else DEFAULT_PSD

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3
MASK64 = (1 << 64) - 1


def fnv1a(data):
    h = FNV_OFFSET
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def unpackbits(src, need):
    out = bytearray()
    i = 0
    n = len(src)
    while len(out) < need:
        if i >= n:
            raise ValueError('RLE exhausted (%d/%d)' % (len(out), need))
        b = src[i]
        i += 1
        if b < 128:
            out += src[i:i + b + 1]
            i += b + 1
        elif b > 128:
            out += bytes([src[i]]) * (257 - b)
            i += 1
    return bytes(out[:need])


def main():
    print('input: %s' % PSD)
    fp = open(PSD, 'rb')
    sig, ver, res, chans, h, w, depth, mode = struct.unpack('>4sH6sHIIHH', fp.read(26))
    assert sig == b'8BPS'
    fp.seek(struct.unpack('>I', fp.read(4))[0], 1)
    fp.seek(struct.unpack('>I', fp.read(4))[0], 1)
    lmi_len = struct.unpack('>I', fp.read(4))[0]
    lmi_start = fp.tell()

    layer_info_len = struct.unpack('>I', fp.read(4))[0]
    layer_count = struct.unpack('>h', fp.read(2))[0]
    print('layer info: len=%d count=%d (transparency=%s)' % (layer_info_len, layer_count, layer_count < 0))
    n = abs(layer_count)

    layers = []
    for _ in range(n):
        top, left, bottom, right = struct.unpack('>4i', fp.read(16))
        nch = struct.unpack('>H', fp.read(2))[0]
        chans_info = []
        for _ in range(nch):
            cid, clen = struct.unpack('>hI', fp.read(6))
            chans_info.append((cid, clen))
        bsig, bkey = struct.unpack('>4s4s', fp.read(8))
        opacity, clipping, flags, filler = struct.unpack('>4B', fp.read(4))
        extra_len = struct.unpack('>I', fp.read(4))[0]
        extra_start = fp.tell()
        # mask data / blending ranges / name / additional infos
        mask_len = struct.unpack('>I', fp.read(4))[0]
        mask = None
        if mask_len >= 20:
            mrect = struct.unpack('>4i', fp.read(16))
            mdef, mflags = struct.unpack('>2B', fp.read(2))
            mask = {'rect': mrect, 'default': mdef, 'flags': mflags}
            if mask_len >= 36:
                fp.read(2)  # real flags + bg
                mask['real_rect'] = struct.unpack('>4i', fp.read(16))
        fp.seek(extra_start + mask_len, 0)
        fp.read(4 + struct.unpack('>I', fp.read(4))[0])  # layer blending ranges
        name_len = fp.read(1)[0]
        name = fp.read(name_len).split(b'\0')[0].decode('latin-1')
        pad = (4 - (name_len % 4)) % 4
        fp.read(pad)
        infos = []
        while fp.tell() < extra_start + extra_len:
            msig = fp.read(4)
            if len(msig) < 4 or msig not in (b'8BIM', b'8B64'):
                break
            ikey = fp.read(4)
            ilen = struct.unpack('>I', fp.read(4))[0]
            idata = fp.read(ilen)
            if ilen % 2:
                fp.read(1)
            infos.append((ikey.decode('latin-1'), ilen, idata))
        fp.seek(extra_start + extra_len, 0)
        layers.append(dict(rect=(top, left, bottom, right), chans=chans_info, blend=bkey.decode('latin-1'),
                           opa=opacity, clip=clipping, flags=flags, name=name, infos=infos, mask=mask))

    # channel image data: one block per channel, in record order
    planes = []
    for L in layers:
        got = {}
        for (cid, clen) in L['chans']:
            start = fp.tell()
            comp = struct.unpack('>H', fp.read(2))[0]
            payload_len = clen - 2
            dims = (L['rect'][3] - L['rect'][1], L['rect'][2] - L['rect'][0])  # w, h
            if cid == -2 and L['mask']:
                mr = L['mask'].get('real_rect') or L['mask']['rect']
                dims = (mr[3] - mr[1], mr[2] - mr[0])
            pw, ph = dims
            if comp == 0:
                data = fp.read(payload_len)
                plane = data[:pw * ph]
            elif comp == 1:
                lens = [struct.unpack('>H', fp.read(2))[0] for _ in range(ph)]
                plane = bytearray()
                for y in range(ph):
                    plane += unpackbits(fp.read(lens[y]), pw)
            else:
                raise SystemExit('compression %d' % comp)
            assert fp.tell() == start + clen, (L['name'], cid, fp.tell(), start + clen)
            got[cid] = (bytes(plane), pw, ph)
        planes.append(got)

    print('%-4s %-38s %-5s %4s %-6s %-9s %s' % ('idx', 'name', 'blend', 'opa', 'flags', 'channels', 'RGBA fnv1a'))
    for i, (L, got) in enumerate(zip(layers, planes)):
        pw = L['rect'][3] - L['rect'][1]
        ph = L['rect'][2] - L['rect'][0]
        rgb = [got.get(c) for c in (0, 1, 2)]
        alpha = got.get(-1)
        ok = all(p is not None and p[1] == pw and p[2] == ph for p in rgb)
        digest = '-'
        if ok:
            buf = bytearray(pw * ph * 4)
            for c in range(3):
                buf[c::4] = rgb[c][0]
            if alpha:
                buf[3::4] = alpha[0]
            digest = '%016x' % fnv1a(buf)
            if L['name'] in want_dump:
                path = '%s/py_layer_%s.rgba' % (OUT_DIR.rstrip('/'), L['name'].replace(' ', '_'))
                with open(path, 'wb') as o:
                    o.write(bytes(buf))
                print('   dumped %s (%d bytes)' % (path, len(buf)))
        print('%-4d %-38s %-5s %4d 0x%02x %-9s %s' % (
            i, L['name'][:38], L['blend'], L['opa'], L['flags'],
            ','.join(str(c) for c, _ in L['chans']) + (' M' if L['mask'] else ''), digest))


main()
