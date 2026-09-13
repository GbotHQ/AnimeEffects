#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# This probe is about the QUALITY INTENT and the codec s own value, which are two
# controls for one number: the intent ("Balanced") is the codec knob seen from
# further away, and the spin box in Advanced is that same knob named as a number.
# While a number is named, the intent is not in charge, and the form has to say so
# by making one of them inert rather than by leaving both live.
#
# The previous version of this probe clicked pixel 462,138 four times to walk a
# popup and asserted that its screenshot was not black: after the export flow was
# restructured it drove no control at all and tested nothing. Everything here
# addresses widgets BY NAME, and the state of the two controls (which one is
# operable) comes from the accessibility tree rather than from a screenshot.
#
# The BODY only navigates and leaves evidence. The assertions run out here.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"
# The revision of the export flow this probe's navigation was recorded against. See
# AE_FLOW_REVISION in _lib.sh: the guard refuses to run a probe that predates it.
export RECORDED_AGAINST=2

rm -f /tmp/qual_form.txt /tmp/qual_advanced.txt /tmp/qual_facts.txt \
      /tmp/qual_state_before.txt /tmp/qual_state_after.txt

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  '"$APP"' "'"$PROJ"'" >/tmp/probe_qual.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # The two controls, and WHICH OF THEM IS OPERABLE. ae_ui.py answers showing /
  # hidden / absent and has no command for the enabled state, so this small query
  # is the one piece of evidence the shared helpers cannot produce; the outer
  # script asserts on it like any other report. It is read from the same
  # accessibility tree by the same lookup (ae_ui.find), never from a pixel.
  quality_state() {  # $1 = the file to write
    python3 - "$1" <<PY 2>/dev/null
import sys, warnings
warnings.filterwarnings("ignore")
sys.path.insert(0, "$ROOT/scripts/gui")
import ae_ui
from gi.repository import Atspi
out = open(sys.argv[1], "w")
for name in ["Quality", "Quality override"]:
    node = ae_ui.find(name, timeout=6.0)
    if node is None:
        out.write("%s: absent\n" % name)
        continue
    enabled = node.get_state_set().contains(Atspi.StateType.ENABLED)
    value = None
    iface = node.get_value_iface()
    if iface is not None:
        value = iface.get_current_value()
    out.write("%s: enabled=%s value=%s\n" % (name, enabled, value))
out.close()
PY
  }

  # 1. A destination first, then the form it dictates. Telegram sticker resolves
  #    its codec (libvpx-vp9) and its quality from the table, so this is the form
  #    in which both the intent and the codec s own value exist.
  ae_press "File" "the File menu opens"
  ae_press "Export project as" "the export entry is there"
  require_export_flow
  ae set "Search destinations" "telegram sticker" >/dev/null
  sleep 1.2
  ae_press "Use this destination" "the Telegram sticker destination is chosen"
  ae wait "Advanced" >/dev/null 2>&1 || true
  sleep 1.5
  ae_report /tmp/qual_form.txt "Destination" "Quality" "Advanced" "Quality override" "Width"
  ae text > /tmp/qual_facts.txt 2>/dev/null
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/qual_01_destination.png

  # 2. Advanced is where the codec s own value lives.
  ae_press "Advanced" "Advanced opens on the codec s own value"
  sleep 1.5
  ae_report /tmp/qual_advanced.txt "Advanced" "Quality" "Quality override" "Quality override to auto"
  quality_state /tmp/qual_state_before.txt
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/qual_02_advanced.png

  # 3. The user names the number themselves, and the intent stops being in charge.
  ae set "Quality override" 40
  sleep 1.5
  quality_state /tmp/qual_state_after.txt
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/qual_03_named.png
  ae_press "Cancel" "the form closes on Cancel"
  sleep 1
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

# ---- the intent is offered, and the codec s own value is one disclosure away ---
assert_showing /tmp/qual_form.txt "Quality" "with a destination chosen the Quality intent is offered"
assert_hidden /tmp/qual_form.txt "Quality override" "the codec s own value stays inside the collapsed Advanced section"
assert_showing /tmp/qual_advanced.txt "Quality override" "and opening Advanced offers the codec s own value"
assert_showing /tmp/qual_advanced.txt "Quality override to auto" "together with the way back to Auto"
assert_rendered "$SHOTS/qual_01_destination.png" "the form carrying the destination s quality intent"
assert_rendered "$SHOTS/qual_02_advanced.png" "and its Advanced section"

# ---- one number, two controls: naming it takes the intent out of charge -------
# The rule the dialog implements (exportdiag.h, applyArtifactConstraints): the
# intent is enabled exactly while the codec s own value is Auto, so a control that
# is inert is the form saying which of the two is in charge.
if grep -q "Quality override: enabled=True" /tmp/qual_state_before.txt; then
    check 0 "the codec s own value is the control that is operable"
else
    check 1 "the codec s own value is the control that is operable (got: $(tr '\n' ' ' < /tmp/qual_state_before.txt 2>/dev/null))"
fi
if grep -q "Quality: enabled=False" /tmp/qual_state_before.txt; then
    check 0 "and while a number stands there the Quality intent is not operable (one number, one control in charge)"
else
    check 1 "and while a number stands there the Quality intent is not operable (got: $(tr '\n' ' ' < /tmp/qual_state_before.txt 2>/dev/null))"
fi
if grep -q "Quality override: value=40.0" /tmp/qual_state_after.txt; then
    check 0 "the number the user names reaches the codec s own value"
else
    check 1 "the number the user names reaches the codec s own value (got: $(tr '\n' ' ' < /tmp/qual_state_after.txt 2>/dev/null))"
fi
if grep -q "Quality: enabled=False" /tmp/qual_state_after.txt; then
    check 0 "and naming it leaves the Quality intent out of charge"
else
    check 1 "and naming it leaves the Quality intent out of charge (got: $(tr '\n' ' ' < /tmp/qual_state_after.txt 2>/dev/null))"
fi
assert_rendered "$SHOTS/qual_03_named.png" "the form after the number was named"

summary probe_quality
