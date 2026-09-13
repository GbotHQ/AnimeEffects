#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# THE FORM'S PER-RUN INPUTS ARE REMEMBERED. The frame range, the size and the frame
# rate used to reset to the project's own values every time the dialog was built, so
# changing one checkbox meant re-selecting all three first - and the dialog is a modal
# you close and reopen constantly while iterating.
#
# What is asserted here is the FORM, read back by name. The version this replaces
# asserted the export's OUTPUT (a 0..3 range writing four files at 160px, then a
# second export writing the same), which cost two real encodes and could not say
# WHICH input had been forgotten when it failed. The write path is covered by
# probe_export and probe_still; the question here is what the reopened form holds.
#
# The save happens when Export is pressed - the inputs are written to QSettings
# before the platform file dialog opens - so the first pass ends by CANCELLING that
# dialog: no encode, and the same settings written. Cancel on the form itself would
# prove nothing, because that path never writes them.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"
# The revision of the export flow this probe's navigation was recorded against. See
# AE_FLOW_REVISION in _lib.sh: the guard refuses to run a probe that predates it.
export RECORDED_AGAINST=2

# A sandboxed profile with the destination already chosen. Export asks where first,
# and the form that follows is the one whose inputs are remembered - so the
# destination has to be stated, and stating it through the settings is how the
# application itself records it (Qt's INI format keeps root keys in [General]).
CFG=$(mktemp -d)
OUT=$(mktemp -d)
export XDG_CONFIG_HOME="$CFG"
mkdir -p "$CFG/AnimeEffectsProject"
printf '[General]\nexport_destination=chat.discord.file\nexport_quality_bucket=1\n' \
    > "$CFG/AnimeEffectsProject/AnimeEffects.conf"
export OUT

rm -f /tmp/inputs_before_values.txt /tmp/inputs_after_values.txt

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  # The value a spin box HOLDS, by name. ae_ui.py reports what is showing and can read
  # a line edit, but this Qt build answers its EditableText interface with an empty
  # string for a QSpinBox, so the number comes from the same Value interface the
  # application itself reads (widthSpinBox->value()). A label that merely contains the
  # words ("- First frame -") is skipped: only a node that HAS a number answers.
  values() { # values <output file> <name>...
    python3 - "$@" <<PY
import os, sys, warnings
warnings.filterwarnings("ignore")
import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi as A

want = os.environ.get("AE_UI_PID", "").strip()
out = open(sys.argv[1], "w")
names = sys.argv[2:]

def walk(node, depth=0):
    if depth > 16:
        return
    try:
        count = node.get_child_count()
    except Exception:
        return
    for i in range(count):
        try:
            child = node.get_child_at_index(i)
        except Exception:
            continue
        yield child
        yield from walk(child, depth + 1)

def application():
    desktop = A.get_desktop(0)
    best = None
    for i in range(desktop.get_child_count()):
        try:
            app = desktop.get_child_at_index(i)
            if "AnimeEffects" not in (app.get_name() or ""):
                continue
            if want and str(app.get_process_id()) != want:
                continue
            if best is None or app.get_child_count() > best.get_child_count():
                best = app
        except Exception:
            continue
    return best

def number(node):
    iface = node.get_value_iface()
    if iface is None:
        return None
    try:
        value = iface.get_current_value()
    except Exception:
        return None
    return str(int(value)) if value == int(value) else str(value)

app = application()
if app is None:
    out.write("NO APPLICATION NODE\n")
    out.close()
    sys.exit(0)
for name in names:
    answer = "absent"
    for node in walk(app):
        try:
            shown = node.get_name() or ""
            described = node.get_description() or ""
        except Exception:
            continue
        if name != shown and name != described and name not in shown:
            continue
        value = number(node)
        if value is None:
            continue
        answer = value
        break
    out.write(name + "=" + answer + "\n")
out.close()
PY
  }

  cd "$OUT"
  '"$APP"' "'"$PROJ"'" >/tmp/probe_inputs.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 13
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # Export asks WHERE first, every time: the picker, then the form.
  open_form() {
    ae_press "File" "the File menu opens"
    ae_press "Export project as" "the export entry is there"
    require_export_flow
    ae wait "Use this destination" >/dev/null 2>&1 || true
    ae_press "Use this destination" "the destination is accepted"
    # The plan for the destination is applied after the form appears, so wait for a
    # widget it only shows once that has happened before reading any input back.
    ae wait "Advanced" >/dev/null 2>&1 || true
    sleep 1.5
  }

  # 1. Set values that are not the project own: the frames 2..7, 160x256, 17 fps.
  open_form
  ae set "First frame" 2 >/dev/null; sleep 0.6
  ae set "Last frame" 7 >/dev/null; sleep 0.6
  ae set "Width" 160 >/dev/null; sleep 0.6
  ae set "Height" 256 >/dev/null; sleep 0.6
  ae set "Frame rate" 17 >/dev/null; sleep 1.2
  values /tmp/inputs_before_values.txt "First frame" "Last frame" "Width" "Height" "Frame rate"
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/inputs_01_set.png

  # 2. Press Export and CANCEL the platform file dialog it opens. No encode happens,
  #    and the inputs are written on the way in - which is what makes the reopen below
  #    a test of the remembering rather than of the encoder.
  ae_press "Export" "the form is answered"
  sleep 3
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/inputs_02_filedialog.png
  xdotool key Escape
  sleep 2.5

  # 3. Reopen the form and touch NOTHING. Whatever the form holds now is what was
  #    remembered.
  open_form
  values /tmp/inputs_after_values.txt "First frame" "Last frame" "Width" "Height" "Frame rate"
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/inputs_03_reopened.png
  ae_press "Cancel" "the form closes on Cancel"
  sleep 1
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

# What the form held on the first pass, which is what has to come back.
expect_value() { # expect_value <file> <name> <value> <what>
    if grep -qxF "$2=$3" "$1"; then
        check 0 "$4"
    else
        check 1 "$4 (got: $(grep -F "$2=" "$1" 2>/dev/null | tr -d '\n'))"
    fi
}
expect_value /tmp/inputs_before_values.txt "First frame" 2 "the first frame can be set on the form"
expect_value /tmp/inputs_before_values.txt "Last frame" 7 "and the last frame"
expect_value /tmp/inputs_before_values.txt "Width" 160 "and the width"
expect_value /tmp/inputs_before_values.txt "Height" 256 "and the height"
expect_value /tmp/inputs_before_values.txt "Frame rate" 17 "and the frame rate"

# THE POINT: the reopened form holds the same numbers, one by one. Comparing the two
# files rather than hardcoding the answers is deliberate - a spin box clamps to what
# the project and the destination allow, and what has to be remembered is what the
# form actually held, not what this probe typed.
for name in "First frame" "Last frame" "Width" "Height" "Frame rate"; do
    before=$(grep -F "$name=" /tmp/inputs_before_values.txt 2>/dev/null | head -1)
    after=$(grep -F "$name=" /tmp/inputs_after_values.txt 2>/dev/null | head -1)
    if [ -n "$before" ] && [ "$before" = "$after" ]; then
        check 0 "the reopened form remembers $name ($before)"
    else
        check 1 "the reopened form remembers $name (was $before, is $after)"
    fi
done

assert_rendered "$SHOTS/inputs_01_set.png" "the form holding the inputs this probe set"
assert_rendered "$SHOTS/inputs_02_filedialog.png" "the platform file dialog Export opened, which is when the inputs are saved"
assert_rendered "$SHOTS/inputs_03_reopened.png" "the reopened form, with its inputs restored"
rm -rf "$CFG" "$OUT"
summary probe_inputs
