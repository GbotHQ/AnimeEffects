#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# THE REPEAT, which is the whole iteration loop of an animation tool: the same sticker
# gets exported a dozen times while the art is adjusted. "Export again" repeats the
# last export - destination, quality, range, size, fps, artifact - WITHOUT re-walking
# the menu and the destination, and asks for a NAME instead.
#
# What this probe establishes is the SHAPE of the second run, which is what the old
# one could not see: the destination picker must NOT be required again (it is asserted
# absent while the repeat is asking for its name), and the run must still reach the
# export path and say what it wrote. The version this replaces clicked recorded pixels
# for the menu row and asserted that the screenshot was not black, so a repeat that had
# quietly started asking for a destination again would have gone on passing.
#
# The destination is preselected through the sandboxed settings rather than by clicking
# a row: choosing one is a typed search in the picker now, and this probe is not about
# that choice (probe_menu and probe_dest are). One real export is needed to enable
# "Export again" at all - it is disabled until an export succeeds - and it is kept as
# short as the form allows: four frames.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"
# The revision of the export flow this probe's navigation was recorded against. See
# AE_FLOW_REVISION in _lib.sh: the guard refuses to run a probe that predates it.
export RECORDED_AGAINST=2

CFG=$(mktemp -d)
OUT=$(mktemp -d)
export XDG_CONFIG_HOME="$CFG"
mkdir -p "$CFG/AnimeEffectsProject"
printf '[General]\nexport_destination=chat.discord.file\nexport_quality_bucket=1\n' \
    > "$CFG/AnimeEffectsProject/AnimeEffects.conf"
export OUT
rm -f "$OUT/repeat1.mp4" "$OUT/repeat2.mp4" /tmp/re_status1.txt /tmp/re_status2.txt /tmp/re_set.txt

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  # The frame range, by name, through the Value interface: ae_ui.py can click what it
  # finds, but this Qt build answers EditableText with an EMPTY string for a QSpinBox,
  # so `ae set` writes text that no value is taken from - and the label beside the box
  # reads "- First frame -", which contains the same words. What is asked for is the
  # node that answers to the name AND HOLDS A NUMBER. (probe_inputs.sh carries the same
  # helper with a reader; it is duplicated rather than shared because the only file the
  # two could share is _lib.sh, which is not this probe to change.)
  set_range() { # set_range <first> <last>
    python3 - "$1" "$2" <<PY
import os, sys, warnings
warnings.filterwarnings("ignore")
import gi
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi as A

want = os.environ.get("AE_UI_PID", "").strip()
wanted = {"First frame": sys.argv[1], "Last frame": sys.argv[2]}

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

app = application()
if app is None:
    print("NO APPLICATION NODE")
    sys.exit(1)
for node in walk(app):
    try:
        shown = node.get_name() or ""
        described = node.get_description() or ""
    except Exception:
        continue
    for name in list(wanted):
        if name != shown and name != described and name not in shown:
            continue
        iface = node.get_value_iface()
        if iface is None:
            continue
        try:
            iface.set_current_value(float(wanted.pop(name)))
            print("set " + name + "=" + str(int(iface.get_current_value())))
        except Exception as exc:
            print("FAILED " + name + " " + str(exc))
            sys.exit(1)
        break
if wanted:
    print("MISSING " + ",".join(wanted))
    sys.exit(1)
PY
  }

  # The application is launched FROM the output directory and the file dialogs are
  # given a bare name: the platform file dialog is a different toolkit in the same
  # process, so its widgets are not in the application tree and it cannot be addressed
  # by name - a relative name resolves against the working directory instead.
  cd "$OUT"
  '"$APP"' "'"$PROJ"'" >/tmp/probe_reexport.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 13
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # The completion signal is what the APP says. A file on disk is not a finished
  # export: the muxer writes its header the moment the export opens, so a 48-byte file
  # sits there long before the export returns.
  wait_for() { # wait_for <name the status bar should name> <output file>
    local status=""
    local i
    for i in $(seq 1 12); do
      status=$(ae label "Exported $1" 2>/dev/null)
      [ -n "$status" ] && break
    done
    printf "%s\n" "$status" > "$2"
  }

  # 1. The first export, through the flow as it is now: WHERE first, then the form.
  ae_press "File" "the File menu opens"
  ae_press "Export project as" "the export entry is there"
  require_export_flow
  ae_press "Use this destination" "the remembered destination is accepted"
  ae wait "Advanced" >/dev/null 2>&1 || true
  sleep 1.5
  # Short, so that the first export is seconds and not minutes: this probe is about the
  # repeat, not about the encode. If the range cannot be set, the body stops HERE
  # rather than encoding the whole project - a probe that quietly ran for a quarter of
  # an hour would be worse than one that fails.
  if set_range 1 4 > /tmp/re_set.txt; then
    check 0 "the exported range is set by name ($(tr -d '\n' < /tmp/re_set.txt))"
  else
    check 1 "the exported range is set by name (see /tmp/re_set.txt: $(tr -d '\n' < /tmp/re_set.txt))"
    xdotool mousemove 700 600; sleep 0.5
    import -window root '"$SHOTS"'/re_00_range_failed.png
    kill $app 2>/dev/null; wait $app 2>/dev/null
    exit 1
  fi
  sleep 1.2
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/re_01_form.png
  ae_press "Export" "the first export is asked for"
  sleep 4
  xdotool key --clearmodifiers ctrl+a; sleep 0.4
  xdotool type --delay 25 "repeat1.mp4"; sleep 0.6
  xdotool key Return
  wait_for "repeat1.mp4" /tmp/re_status1.txt
  sleep 1
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/re_02_first_done.png

  # 2. Export again. It must ask for a NAME and not for a destination: no picker, and
  #    the run still reaches the export path. "File" is asked about as well, because a
  #    report answers as soon as ANY name it was asked about is up - naming something
  #    that is on screen either way keeps the two absences from costing the full report
  #    timeout each.
  ae_press "File" "the File menu opens for the repeat"
  ae_press "Export again" "Export again is enabled by the first export"
  sleep 4
  ae_report /tmp/re_repeat.txt "Search destinations" "Use this destination" "File"
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/re_03_name_prompt.png
  xdotool key --clearmodifiers ctrl+a; sleep 0.4
  xdotool type --delay 25 "repeat2.mp4"; sleep 0.6
  xdotool key Return
  wait_for "repeat2.mp4" /tmp/re_status2.txt
  sleep 1
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/re_04_again_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

# The first export has to have SUCCEEDED - that is what enables Export again, and the
# application names the file it wrote when the export returns.
if grep -q "Exported repeat1.mp4" /tmp/re_status1.txt; then
    check 0 "the first export reported the file it wrote ($(head -c 80 /tmp/re_status1.txt))"
else
    check 1 "the first export reported the file it wrote (status bar said: $(tr -d '\n' < /tmp/re_status1.txt))"
fi
# THE POINT OF THE PROBE: the repeat did not re-walk the destination flow.
assert_absent /tmp/re_repeat.txt "Search destinations" "Export again asks for no destination: the picker is not up"
assert_absent /tmp/re_repeat.txt "Use this destination" "not even its answer button, so the picker was not required"
# ...and it still reached the export path, which is what it says afterwards. A run that
# quietly did nothing would leave the status bar empty and no file on disk.
if grep -q "Exported repeat2.mp4" /tmp/re_status2.txt; then
    check 0 "and the repeat reported the file it wrote ($(head -c 80 /tmp/re_status2.txt))"
else
    check 1 "and the repeat reported the file it wrote (status bar said: $(tr -d '\n' < /tmp/re_status2.txt))"
fi
assert_exists "$OUT/repeat2.mp4" "the repeat wrote the file it was asked for"
assert_exists "$OUT/repeat1.mp4" "and the first export is still there beside it"
assert_rendered "$SHOTS/re_01_form.png" "the form on the first export"
assert_rendered "$SHOTS/re_03_name_prompt.png" "the name prompt Export again opened instead of a destination picker"
assert_rendered "$SHOTS/re_04_again_done.png" "the application after the repeat"
rm -rf "$CFG" "$OUT"
summary probe_reexport
