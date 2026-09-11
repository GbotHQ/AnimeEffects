#!/bin/bash
# Run every GUI probe and report. These are the end-to-end checks that no unit
# test covers: the export dialog's wiring, the quick-export menu, the OS file
# dialog, and whether an export actually lands inside a platform's cap.
#
# They need a display (Xvfb is created per probe), a built Debug binary, and a
# real .anie project. Pass one, or set AE_PROJECT, or let them search.
#
#   scripts/gui/run_probes.sh [project.anie]
set -u
DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJ="${1:-${AE_PROJECT:-}}"

pass=0; fail=0; failed=""
for probe in "$DIR"/probe_*.sh; do
    name=$(basename "$probe")
    # probe_capped exports up to 16 encodes; give the slow ones room.
    out=$(timeout 900 "$probe" "$PROJ" 2>&1)
    if [ $? -eq 0 ]; then
        pass=$((pass+1)); echo "PASS  $name"
    else
        fail=$((fail+1)); failed="$failed $name"
        echo "FAIL  $name"
        echo "$out" | grep -E "^(PASS|FAIL)" | sed 's/^/      /'
    fi
done
echo "=== probes: pass=$pass fail=$fail${failed:+ failed:$failed} ==="
exit $((fail > 0 ? 1 : 0))
