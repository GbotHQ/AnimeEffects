#!/bin/bash
# Run every test target the CURRENT CMakeLists declares.
#
# Deliberately not `for t in build/tests/*/`: a stale binary left behind by a
# test whose source has since been deleted passes happily and reports a green
# suite that tests nothing. The declared list is the source of truth.
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CFG=${1:-Debug}
# Target names come from `add_executable(<name> ...)`; keep only those that are
# actual test programs (all of them, minus the interactive/arg-requiring ones).
declare -A SKIP=(
  [media_exporter_e2e]=1   # needs Release + xcb + a real .anie
  [roundtrip]=1            # needs .anie arguments
  [easing_picker_shot]=1   # writes screenshots
  # picker_probe used to be skipped here. It constructs widgets (no display needed:
  # it runs under QT_QPA_PLATFORM=offscreen) and it now EXITS NON-ZERO on a
  # mismatch, so it is a gate: it reported a stale height expectation for months
  # while exiting 0.
  [picker_pos_probe]=1
  [media_link_probe]=1
)
targets=$(grep -oP '^add_executable\(\K[A-Za-z0-9_]+' tests/CMakeLists.txt | sort -u)
pass=0; fail=0; failed=""
for n in $targets; do
  [ -n "${SKIP[$n]:-}" ] && continue
  bin=build/tests/$CFG/$n
  if [ ! -x "$bin" ]; then echo "MISSING $n"; fail=$((fail+1)); failed="$failed $n(missing)"; continue; fi
  out=$(QT_QPA_PLATFORM=offscreen timeout 180 "$bin" 2>&1); rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); echo "PASS $n";
  else fail=$((fail+1)); failed="$failed $n"; echo "FAIL($rc) $n"; echo "$out" | tail -10; fi
done
echo "=== $CFG: pass=$pass fail=$fail failed:$failed ==="
