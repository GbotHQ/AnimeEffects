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
pass=0; fail=0; skipped=""; failed=""
for n in $targets; do
  [ -n "${SKIP[$n]:-}" ] && continue
  bin=build/tests/$CFG/$n
  if [ ! -x "$bin" ]; then echo "MISSING $n"; fail=$((fail+1)); failed="$failed $n(missing)"; continue; fi
  if [ "$n" = media_exporter_e2e ]; then
    # The end-to-end export harness needs a real GL context and a loaded project, so it
    # runs under Xvfb against the COMMITTED fixture (tests/fixtures/e2e_stage.png) rather
    # than being skipped for want of artwork on the machine. It used to be in the SKIP
    # list above, which meant 45 assertions - the only ones that drive ctrl::Exporter
    # through the real exporter wiring - ran when a developer remembered to run them.
    # Override the project with AE_E2E_PROJECT (e.g. a real .anie); the suite is a gate
    # either way. xcb and an UNSET WAYLAND_DISPLAY are both required: with Wayland set,
    # Qt prefers it and the GL widget never gets a context.
    if ! command -v xvfb-run >/dev/null 2>&1; then
      echo "SKIP $n (no xvfb-run; needs a GL context)"; skipped="$skipped $n(no-xvfb)"
      continue
    fi
    proj="${AE_E2E_PROJECT:-tests/fixtures/e2e_stage.png}"
    out=$(env -u WAYLAND_DISPLAY QT_QPA_PLATFORM=xcb timeout 600 \
              xvfb-run -a "$bin" "$proj" 2>&1); rc=$?
  else
    out=$(QT_QPA_PLATFORM=offscreen timeout 180 "$bin" 2>&1); rc=$?
  fi
  if [ $rc -eq 0 ]; then pass=$((pass+1)); echo "PASS $n";
  else fail=$((fail+1)); failed="$failed $n"; echo "FAIL($rc) $n"; echo "$out" | tail -10; fi
done
echo "=== $CFG: pass=$pass fail=$fail skipped:$skipped failed:$failed ==="
