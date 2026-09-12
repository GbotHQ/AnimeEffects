#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# The two image artifacts, end to end through the real app:
#
#   still     ONE image file at the name the user typed. Posting an illustration
#             is the common case for this audience, and it is the opposite of what
#             every other destination does: the save dialog asks for a file and the
#             file it is given is what must appear, not `name0001.png` beside it.
#   sequence  a directory of numbered stills. pixiv's ugoira format is exactly
#             this, and it is deliberately NOT a video and NOT a GIF.
#
# Both are produced by Qt's image writers inside ctrl::Exporter, so a probe that
# only checked the plan would miss the file naming, which is the whole promise.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

OUTDIR=$(mktemp -d)
CFG=$(mktemp -d)
export XDG_CONFIG_HOME="$CFG"
mkdir -p "$CFG/AnimeEffectsProject"
BASE=$(basename "${PROJ%.*}")

# run_case <destination id> <typed name> <what the export should leave behind>
start_export() {
    local dest="$1"
    printf '[General]\nexport_destination=%s\nexport_quality_bucket=1\n' "$dest" \
        > "$CFG/AnimeEffectsProject/AnimeEffects.conf"
}

# ---------------------------------------------------------------- a still image
start_export still.pixiv.illustration
rm -f "$OUTDIR/illustration.png" "$OUTDIR"/illustration[0-9]*.png
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "'"$OUTDIR"'" && '"$APP"' "'"$PROJ"'" >/tmp/probe_still.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 5
  require_export_flow
  import -window root "$SHOTS"/still_01_dialog.png
  # Export (destination mode puts the button here), then name the image.
  xdotool key alt+e; sleep 4
  xdotool mousemove 400 25 click 1; sleep 0.4
xdotool key ctrl+a; sleep 0.3
  xdotool type --delay 25 "illustration.png"; sleep 0.5
  xdotool key Return
  sleep 40
  import -window root "$SHOTS"/still_02_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -2

assert_exists "$OUTDIR/illustration.png" "the still destination wrote the name it was given"
# The single-file promise: no numbered sibling.
ls "$OUTDIR"/illustration[0-9]*.png >/dev/null 2>&1
[ $? -ne 0 ]; check $? "no numbered sequence was written beside it"
if [ -s "$OUTDIR/illustration.png" ]; then
    fmt=$(magick identify -format "%m %wx%h" "$OUTDIR/illustration.png" 2>/dev/null)
    echo "      PNG check: $fmt"
    case "$fmt" in PNG*) check 0 "the file is a PNG ($fmt)";; *) check 1 "the file is a PNG ($fmt)";; esac
fi
assert_rendered "$SHOTS/still_02_done.png" "the app after the still export"

# ----------------------------------------------------------- a frame sequence
start_export post.pixiv.ugoira
rm -f "$OUTDIR"/ugoira*.png
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "'"$OUTDIR"'" && '"$APP"' "'"$PROJ"'" >/tmp/probe_ugoira.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 5
  # Shrink the frame so 150 of them render quickly. A sequence is a `free` size
  # destination, so its Width/Height boxes are live: the frame range is NOT
  # (ugoira caps the project range at 150 stills), which is why the size and not
  # the range is what this probe sets.
  xdotool mousemove 335 341 click 1; sleep 0.6
  xdotool key ctrl+a; sleep 0.2; xdotool type --delay 30 "160"; sleep 0.3
  xdotool mousemove 335 376 click 1; sleep 0.6
  xdotool key ctrl+a; sleep 0.2; xdotool type --delay 30 "256"; sleep 0.3
  xdotool key Tab; sleep 1.0
  import -window root "$SHOTS"/ugoira_01_dialog.png
  xdotool key alt+e; sleep 4
  xdotool mousemove 400 25 click 1; sleep 0.4
xdotool key ctrl+a; sleep 0.3
  xdotool type --delay 25 "ugoira.png"; sleep 0.5
  xdotool key Return
  sleep 150
  import -window root "$SHOTS"/ugoira_02_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -2

count=$(ls "$OUTDIR"/ugoira*.png 2>/dev/null | wc -l)
total=$(du -cb "$OUTDIR"/ugoira*.png 2>/dev/null | tail -1 | cut -f1)
echo "      numbered files found: $count, total ${total:-0} bytes"
# 150 is ugoira's own cap on the project's 601-frame range, so the plan had to
# trim rather than hand pixiv a folder it refuses.
[ "$count" = "150" ]
check $? "the sequence wrote ugoira's 150-frame cap (found $count)"
assert_exists "$OUTDIR/ugoira0001.png" "the first frame is numbered from 0001"
assert_exists "$OUTDIR/ugoira0150.png" "the last frame of the cap is there"
[ "${total:-0}" -le 31457280 ] && [ "${total:-0}" -gt 0 ]
check $? "the whole set is inside the 30 MB cap (${total:-0} bytes)"
assert_rendered "$SHOTS/ugoira_02_done.png" "the app after the sequence export"

rm -rf "$OUTDIR" "$CFG"
summary probe_still
