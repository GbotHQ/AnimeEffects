"""Estimates the memory a file will cost when imported, WITHOUT loading it.

The probe imports a whole PSD in one process, so its peak RSS is driven by the total AREA of the
layers (every layer is decoded to a padded RGBA buffer and kept as a resource), not by the file
size. One corpus file reached 11.7 GB that way. This walks only the layer records, which sit at the
head of the file, and reports the area, so a batch can pick a subset that fits the machine.

usage: psd_layer_area.py <file.psd> ...
"""
import os
import struct
import sys


def records(path):
    with open(path, 'rb') as f:
        head = f.read(26)
        if len(head) < 26 or head[:4] != b'8BPS':
            return None
        f.seek(26)
        n = struct.unpack('>I', f.read(4))[0]
        f.seek(n, 1)
        n = struct.unpack('>I', f.read(4))[0]
        f.seek(n, 1)
        lm = struct.unpack('>I', f.read(4))[0]
        blob = f.read(min(lm, 64 << 20))          # records only, never the channel data
    o = 4
    count = struct.unpack_from('>h', blob, o)[0]
    o += 2
    out = []
    for _ in range(abs(count)):
        if o + 16 > len(blob):
            break
        top, left, bottom, right = struct.unpack_from('>iiii', blob, o)
        o += 16
        nch = struct.unpack_from('>H', blob, o)[0]
        o += 2 + 6 * nch
        if o + 12 > len(blob):
            break
        o += 4 + 4 + 4                              # '8BIM', blend key, opacity/clip/flags/filler
        extra, o = struct.unpack_from('>I', blob, o)[0], o + 4
        end = o + extra
        w, h = max(0, right - left), max(0, bottom - top)
        out.append(w * h)
        o = end
    return out


def main():
    print('%-12s %8s %6s %12s %12s  %s' % ('file', 'MB', 'layers', 'layer Mpx', 'predicted GB', 'verdict'))
    for path in sys.argv[1:]:
        try:
            areas = records(path)
        except Exception as e:
            print('%-12s  %s' % (path.split('/')[-1], e))
            continue
        if areas is None:
            continue
        mpx = sum(areas) / 1e6
        # 12 bytes per layer pixel, CALIBRATED rather than guessed: 000061.psd (395 layers,
        # 962.9 Mpx of layer area) was observed at 11.66 GB resident, i.e. ~13 B/px. The cost is
        # the decoded RGBA buffer (4) + the GL texture (4) + the mesh and padding (4+).
        gb = (mpx * 1e6 * 12.0) / 2**30
        if os.path.getsize(path) > 200 * 2**20:
            gb += os.path.getsize(path) / 2**30 * 0.5   # the compressed channels stay resident too
        verdict = 'ok' if gb < 4 else ('heavy: needs an idle machine' if gb < 8 else 'SKIP: too big for this machine')
        print('%-12s %8.0f %6d %12.1f %12.2f  %s' % (
            path.split('/')[-1], os.path.getsize(path) / 2**20, len(areas), mpx, gb, verdict))


if __name__ == '__main__':
    main()
