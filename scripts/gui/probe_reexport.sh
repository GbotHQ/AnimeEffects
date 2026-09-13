#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# THE REPEAT, which is the whole iteration loop of an animation tool: the same sticker
# is exported a dozen times while the art is adjusted. "Export again" repeats the last
# export - destination, quality, range, size, fps, artifact - WITHOUT re-walking the
# menu and the destination, and asks for a NAME instead.
#
# What this probe establishes is the SHAPE of the second run, because that is what the
# old one could not see: the destination picker must NOT be required again (it is
# asserted absent while the repeat is asking for its name), while the run must still
# reach the export path and say what it wrote. The version this replaces clicked
# recorded pixels for the menu row and asserted the screenshot was not black, so a
# repeat that had silently started asking for a destination again would have passed.
#
# The destination is preselected through the sandboxed settings rather than by
# clicking a menu row: a row is chosen by typing it in the picker now, and this probe
# is not about that choice (probe_menu and probe_dest are). One real export is needed
# to enable "Export again" at all - it is disabled until an export succeeds - and it is
# kept as short as the form allows: four frames at 160x120.
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
rm -f "$OUT/repeat1.mp4" "$OUT/repeat2.mp4" /tmp/re_status1.txt /tmp/re_status2.txt

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
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
  # export: the muxer writes its header the moment the export opens.
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
  # repeat, not about the encode.
  ae set "First frame" 1 >/dev/null; sleep 0.6
  ae set "Last frame" 4 >/dev/null; sleep 0.6
  ae set "Width" 160 >/dev/null; sleep 0.6
  ae set "Height" 120 >/dev/null; sleep 1.2
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
  #    that is on screen either way keeps the two absences from costing the full
  #    report timeout each.
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
# app names the file it wrote when the export returns.
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
