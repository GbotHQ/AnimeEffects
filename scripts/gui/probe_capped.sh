#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# A BYTE CAP IS A HARD UPLOAD REJECTION, NOT A PREFERENCE, and the only way to check
# that the file the app produced respects one is to weigh the file. So this probe runs
# a real capped export, end to end, and puts the result on a scale.
#
# What it replaces, and why every step is by name. The destination used to be
# preselected by writing QSettings from the probe, because the quick-export menu is
# grouped by family and "the third row" stopped being Telegram when a family was added
# in front of it: an INDEX into a reordered list is not a destination, and the probe
# exported a chat video while asserting a sticker's name. The body then clicked
# recorded pixels ((81,162) for the export entry), which the destination-first rewrite
# moved, so it drove a flow that no longer existed and still called itself a cap check.
# The destination is chosen in the picker by its own id now and the export goes through
# the form the destination dictates.
#
# ONE capped destination, deliberately. The fit loop re-encodes until the file fits, so
# each case costs minutes; the case worth its minutes is a capped artifact the fit loop
# used to know nothing about (it enforced caps on the video path only, so a GIF went out
# at whatever size it came to).
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"
# The revision of the export flow this probe's navigation was recorded against. See
# AE_FLOW_REVISION in _lib.sh: the guard refuses to run a probe that predates it.
export RECORDED_AGAINST=2

# Telegram's sticker row: WebM/VP9 with alpha, 512 px, three seconds, 256 KB. The plan
# trims the duration and the frame rate itself, and the fit-to-cap loop then has to
# reach 262144 bytes without the user doing anything - which is what is measured here.
DEST_ID="telegram.sticker"
DEST_LABEL="Telegram sticker"
CAP=262144
CAP_KB=256

# THROWAWAY OUTPUT, never the checkout: the platform file dialog resolves a relative
# name against the application's working directory, so the app is launched from here
# and the name typed below lands here.
OUTDIR="$AE_PROBE_DIR"
OUTNAME="probe_capped.webm"
OUT="$OUTDIR/$OUTNAME"
rm -f "$OUT"
rm -f /tmp/cap_form.txt /tmp/cap_facts.txt /tmp/cap_status.txt

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  # cd as its OWN command, not as `cd X && App &`: a background job whose body is a
  # compound command is a SUBSHELL, so $! names the subshell rather than the
  # application, AE_UI_PID matches no accessible root, and every by-name helper answers
  # "no widget named ..." while a window is plainly on screen.
  cd "'"$OUTDIR"'"
  '"$APP"' "'"$PROJ"'" >/tmp/probe_capped.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # 1. The destination, by ID, through the picker the flow opens first. The id is
  #    unique: a search for the label matches several rows, and the picker answers with
  #    whichever the tree lists first.
  ae_press "File" "the File menu opens"
  ae_press "Export project as" "the export entry is there"
  require_export_flow
  ae set "Search destinations" "'"$DEST_ID"'" >/dev/null
  sleep 1.2
  ae_press "Use this destination" "the capped destination is accepted"
  ae wait "Advanced" >/dev/null 2>&1 || true
  sleep 1.5
  ae_report /tmp/cap_form.txt "Destination" "Width" "Height" "Frame rate" "First frame" "Last frame"
  # The cap as the form states it, and the size and rate the plan resolved from it.
  ae text > /tmp/cap_facts.txt 2>/dev/null
  import -window root '"$SHOTS"'/cap_01_form.png

  # 2. Export, and name the file in the platform file dialog - the one step that cannot
  #    be driven by name, because that dialog is a different toolkit in the same
  #    process and its widgets are not in the accessibility tree of the application. It
  #    opens with the name field focused and the suggested name selected.
  ae_press "Export" "the export form is answered"
  sleep 4
  xdotool key --clearmodifiers ctrl+a; sleep 0.4
  xdotool type --delay 25 "'"$OUTNAME"'"; sleep 0.6
  xdotool key Return

  # 3. The completion signal is what the APP says, not the size on disk: the fit loop
  #    rewrites the file on every attempt (and removes the previous attempt first), so
  #    a file that exists - or one whose size has stopped changing - is not a finished
  #    export. The status bar names the file and measures it against the cap when the
  #    export RETURNS; ae label finds it by name and prints the whole message.
  status=""
  for i in $(seq 1 45); do
    status=$(ae label "Exported '"$OUTNAME"'" 2>/dev/null)
    [ -n "$status" ] && break
  done
  echo "waited up to $((i * 10))s for the export to report"
  printf "%s\n" "$status" > /tmp/cap_status.txt
  sleep 1
  import -window root '"$SHOTS"'/cap_02_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

# The form the capped destination dictated, and the cap it says it will be held to. A
# cap that is stated but never applied is the bug this probe exists for, so both halves
# are asked for: the numbers on the form, and the bytes on disk.
assert_showing /tmp/cap_form.txt "Destination" "the export form states the capped destination it was given"
assert_showing /tmp/cap_form.txt "Width" "the destination's own size reaches the form"
assert_showing /tmp/cap_form.txt "Height" "both dimensions of it"
if grep -q "$CAP_KB KB" /tmp/cap_facts.txt; then
    check 0 "the form states the cap this export will be held to ($CAP_KB KB)"
else
    check 1 "the form states the cap this export will be held to ($CAP_KB KB) (got: $(tr '\n' ' ' < /tmp/cap_facts.txt 2>/dev/null | head -c 200))"
fi

# THE ASSERTION THIS PROBE EXISTS FOR: the file on disk, weighed.
assert_under "$OUT" "$CAP" "the exported sticker is inside its $CAP_KB KB cap"
# ...and it is a real export rather than a file the loop left half-written: the fit is a
# bisection, and a bisection that stops on the wrong probe leaves the PREVIOUS attempt
# on disk (the app could report "fitted at 119199 bytes" over a 120687-byte file).
if [ -s "$OUT" ]; then
    frames=$(ffprobe -v error -select_streams v -show_entries stream=nb_frames -of csv=p=0 "$OUT" 2>/dev/null | head -1)
    [ -n "$frames" ] && [ "$frames" -gt 1 ] 2>/dev/null
    check $? "the fitted file still animates (nb_frames=${frames:-none}; one frame would be a still)"
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT" 2>/dev/null | head -1)
    ok=$(awk -v d="${dur:-0}" 'BEGIN { print (d > 0.5 && d <= 3.05) ? 0 : 1 }')
    check "$ok" "and it is inside the destination's own 3 s duration limit (${dur:-none} s)"
fi
# The app's verdict, from the status bar: it names the file, states the size in KB, and
# measures it against the cap. A file that went out over the limit would be reported
# with "; OVER THE LIMIT, it will be rejected" instead, which is asserted against below.
if grep -q "Exported $OUTNAME" /tmp/cap_status.txt; then
    check 0 "the status bar reports the export by name ($(tr -d '\n' < /tmp/cap_status.txt | head -c 120))"
else
    check 1 "the status bar reports the export by name (got: $(tr -d '\n' < /tmp/cap_status.txt 2>/dev/null | head -c 120))"
fi
grep -q "of $CAP_KB KB allowed for $DEST_LABEL" /tmp/cap_status.txt
check $? "and measures it against the cap of the destination the form was given"
grep -q "OVER THE LIMIT" /tmp/cap_status.txt
[ $? -ne 0 ]
check $? "and does not report it as over the limit"
grep -qF "$OUTDIR" /tmp/cap_status.txt
check $? "and says it wrote into the throwaway directory, not the checkout"
assert_rendered "$SHOTS/cap_01_form.png" "the export form carrying the capped destination"
assert_rendered "$SHOTS/cap_02_done.png" "the app after the capped export"
summary probe_capped
