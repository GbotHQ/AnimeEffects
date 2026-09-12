#!/usr/bin/env bash
# Renders the ratio variant at OTHER anchors, on the files where the ratio variant makes the most
# VISIBLE difference - so the anchor question is answered from renders of the files where it matters
# instead of from all 30 files x every anchor (which would be hours for nothing).
#
# usage: anchor_pass.sh <batchDir> <corpus-list> <anchor> [<anchor> ...]
#
# The comparison is against the same batch directory, so the existing const10 / nocut / mean / ratio renders
# renders are kept (SKIP_EXISTING=1) and only the new anchor directories are rendered.
#
# WAIT_PID=<pid> makes this wait for a running pass to finish first: only one probe at a time, because
# a corpus PSD holds gigabytes of decoded layers and two probes at once have OOMed this machine.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
BATCH="$1"; LIST="$2"; shift 2
ANCHORS=("$@")
TOP="${ANCHOR_TOP:-10}"

if [[ -n "${WAIT_PID:-}" ]]; then
    while kill -0 "$WAIT_PID" 2>/dev/null; do sleep 60; done
    echo "waited for pid $WAIT_PID; starting the anchor pass $(date +%H:%M:%S)"
fi

# rank files by the VARIANT's visible change (pixels moving more than 8 levels)
python3 - "$BATCH" "$LIST" "$TOP" <<'PY' > /tmp/anchor_subset.txt
import os, sys
sys.path.insert(0, 'tools/psd_composite_probe')
from cutoff_report import load
import numpy as np
batch, listPath, top = sys.argv[1], sys.argv[2], int(sys.argv[3])
paths = {}
for line in open(listPath):
    line = line.strip()
    if line:
        paths[os.path.basename(line)[:-4]] = line
rows = []
for name in sorted(os.listdir(batch)):
    offPath = os.path.join(batch, name, 'off', 'render_straight.png')
    if not os.path.exists(offPath) or name not in paths:
        continue
    off = load(offPath)
    best = 0
    for variant in sorted(os.listdir(os.path.join(batch, name))):
        p = os.path.join(batch, name, variant, 'render_straight.png')
        if variant == 'off' or not os.path.exists(p):
            continue
        var = load(p)
        if var.shape != off.shape:
            continue
        best = max(best, int((np.abs(off - var).max(axis=2) > 8).sum()))
    rows.append((best, name))
rows.sort(reverse=True)
for best, name in rows[:top]:
    print(paths[name])
    sys.stderr.write('  %s: %d px move by more than 8 levels\n' % (name, best))
PY
echo "anchor pass on $(wc -l < /tmp/anchor_subset.txt) files: $(tr '\n' ' ' < /tmp/anchor_subset.txt)"
SKIP_EXISTING=1 AE_MIN_FREE_GB="${AE_MIN_FREE_GB:-8}" \
    tools/psd_composite_probe/cutoff_batch.sh /tmp/anchor_subset.txt "$BATCH" "${ANCHORS[@]}"
