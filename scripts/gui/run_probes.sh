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

pass=0; fail=0; stale=0; failed=""; stalled=""
for probe in "$DIR"/probe_*.sh; do
    name=$(basename "$probe")
    # probe_capped exports up to 16 encodes; give the slow ones room.
    out=$(timeout 900 "$probe" "$PROJ" 2>&1)
    rc=$?
    # 3 = the probe's navigation no longer matches the application (it exits with
    # its own message). Neither a pass nor a failure of the application: a probe
    # that cannot reach the screen it is meant to assert on tests nothing, and
    # calling that a pass is how ten of these went green after the export form was
    # restructured. Reported as its own state so the debt stays visible.
    if [ $rc -eq 3 ]; then
        stale=$((stale+1)); stalled="$stalled $name"; echo "STALE $name"; continue
    fi
    if [ $rc -eq 0 ]; then
        pass=$((pass+1)); echo "PASS  $name"
    else
        fail=$((fail+1)); failed="$failed $name"
        echo "FAIL  $name"
        echo "$out" | grep -E "^(PASS|FAIL)" | sed 's/^/      /'
    fi
done
echo "=== probes: pass=$pass fail=$fail stale=$stale${failed:+ failed:$failed}${stalled:+ stale:$stalled} ==="
exit $((fail > 0 ? 1 : 0))
