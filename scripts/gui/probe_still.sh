#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# THE TWO IMAGE ARTIFACTS, end to end through the real app:
#
#   still     ONE image file at the name the user typed. Posting an illustration is
#             the common case for this audience, and it is the opposite of what every
#             other destination does: the save dialog asks for a file and the file it
#             is given is what must appear, not `name0001.png` beside it. What the
#             form must NOT ask for is a frame rate or a frame range: a still has no
#             timeline, and two questions that cannot change the answer are the same
#             disease as hiding a control that can.
#   sequence  a directory of numbered stills. pixiv's ugoira format is exactly this,
#             and it is deliberately NOT a video and NOT a GIF: the frame range IS an
#             input (the format caps it at 150 frames), the frame RATE is not, and the
#             numbering is the whole promise.
#
# Both are produced by Qt's image writers inside ctrl::Exporter, so a probe that only
# checked the plan would miss the file naming, which is the whole promise.
#
# What this replaces. The still case used to click recorded pixels (16,10 for the
# export entry, 81,162 for the menu item) and to assert that the screenshot was not
# black; the sequence case clicked (335,341) and (335,376) for the size boxes. Export
# opens the destination picker FIRST now, so the menu coordinates belong to a flow
# that no longer exists, and every control below is addressed by its accessible name
# instead. The destinations are chosen by ID: their labels share words ("pixiv") and a
# search that matches several rows selects whichever the tree lists first.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"
# The revision of the export flow this probe's navigation was recorded against. See
# AE_FLOW_REVISION in _lib.sh: the guard refuses to run a probe that predates it.
export RECORDED_AGAINST=2

# THROWAWAY OUTPUT, never the checkout: the file dialog resolves a relative name
# against the application's working directory, so the app is launched from here and
# the names typed below land here.
OUTDIR="$AE_PROBE_DIR"
STILL_NAME="probe_still.jpg"
STILL="$OUTDIR/$STILL_NAME"
SEQ_NAME="probe_frames.png"
SEQBASE="${SEQ_NAME%.*}"
SEQ_FRAMES=20
rm -f "$STILL" "$OUTDIR/$SEQBASE"*.png
rm -f /tmp/still_form.txt /tmp/still_facts.txt /tmp/still_status.txt
rm -f /tmp/seq_form.txt /tmp/seq_facts.txt /tmp/seq_status.txt

# ------------------------------------------------------------------ a still image
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  # cd as its OWN command, never `cd X && App &`: that backgrounds a SUBSHELL, so $!
  # is not the application and AE_UI_PID addresses an accessible root that has no
  # tree - every by-name query then answers "no widget named ..." while a window is
  # plainly on screen. The previous version of this probe did exactly that.
  cd "'"$OUTDIR"'"
  '"$APP"' "'"$PROJ"'" >/tmp/probe_still.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # 1. The destination, by id, in the picker the flow opens first. Instagram feeds
  #    are the general case for a still: JPEG only, which is the artifact this checks.
  ae_press "File" "the File menu opens"
  ae_press "Export project as" "the export entry is there"
  require_export_flow
  ae set "Search destinations" "still.instagram.feed" >/dev/null
  sleep 1.2
  ae_press "Use this destination" "the still destination is accepted"
  ae wait "Advanced" >/dev/null 2>&1 || true
  sleep 1.5

  # 2. What the form asks a still for: a destination, the image quality the Qt image
  #    writer has, and a size. NOT a frame rate and NOT a frame range - and the
  #    Format row is the destination's fact, not a control.
  ae_report /tmp/still_form.txt "Destination" "Image quality" "Width" "Height" \
                                "Frame rate" "First frame" "Last frame" "Format" "Quality"
  ae text > /tmp/still_facts.txt 2>/dev/null
  import -window root '"$SHOTS"'/still_01_form.png

  # 3. Export, and name the image in the platform file dialog. It is the one step that
  #    cannot be driven by name: it is a different toolkit in the same process, so its
  #    widgets are not in the application accessibility tree. It opens with the name
  #    field focused and the suggested name selected, and a relative name resolves
  #    against the application's working directory.
  ae_press "Export" "the export form is answered"
  sleep 4
  xdotool key --clearmodifiers ctrl+a; sleep 0.4
  xdotool type --delay 25 "'"$STILL_NAME"'"; sleep 0.6
  xdotool key Return
  # The completion signal is what the APP says: the status bar names the image it wrote
  # when the export returns. ae label finds it by name and prints the whole message,
  # which carries the file name, its size, and the directory it landed in.
  status=""
  for i in $(seq 1 30); do
    status=$(ae label "Exported '"$STILL_NAME"'" 2>/dev/null)
    [ -n "$status" ] && break
  done
  printf "%s\n" "$status" > /tmp/still_status.txt
  sleep 1
  import -window root '"$SHOTS"'/still_02_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

# The form a still gets: the destination it was given, the one quality control Qt's
# image writer has, and the size.
assert_showing /tmp/still_form.txt "Destination" "the still form states the destination it was given"
assert_showing /tmp/still_form.txt "Image quality" "a still offers the image writer's own quality control"
assert_showing /tmp/still_form.txt "Width" "and the size the image will be written at"
assert_showing /tmp/still_form.txt "Height" "both dimensions of it"
# ...and what it must NOT ask for. These are hidden rather than greyed: a control that
# cannot affect the file is not information.
assert_hidden /tmp/still_form.txt "Frame rate" "one image has no frame rate to ask for"
assert_hidden /tmp/still_form.txt "First frame" "and no frame range"
assert_hidden /tmp/still_form.txt "Last frame" "neither end of one"
assert_hidden /tmp/still_form.txt "Format" "and the destination dictates the format, so there is no Format control"
# The destination's own decision, in the words on screen: one JPG, not a sequence.
if grep -q "one JPG image" /tmp/still_facts.txt; then
    check 0 "the facts state that this destination writes one image (Artifact: one JPG image)"
else
    check 1 "the facts state that this destination writes one image (got: $(tr '\n' ' ' < /tmp/still_facts.txt 2>/dev/null | head -c 200))"
fi

# THE FILE, at the name the user typed, and ONE of it.
assert_exists "$STILL" "the still destination wrote the name it was given"
siblings=$(ls "$OUTDIR/$SEQBASE"[0-9]*.png "$OUTDIR/probe_still"[0-9]* 2>/dev/null | wc -l)
[ "$siblings" = "0" ]
check $? "no numbered sequence was written beside it ($siblings numbered files)"
if [ -s "$STILL" ]; then
    fmt=$(magick identify -format "%m %wx%h" "$STILL" 2>/dev/null)
    echo "      image check: $fmt"
    # magick reports the format as "JPEG", so the pattern has to be the one it
    # prints rather than the extension on disk.
    case "$fmt" in JPEG*) check 0 "and it is a JPEG, which is what Instagram accepts ($fmt)";;
                  *)      check 1 "and it is a JPEG, which is what Instagram accepts ($fmt)";; esac
fi
# What the app said when it finished: the status bar names the image, its size, and the
# directory it went to. A probe that exported somewhere other than its throwaway
# directory fails here as well as at the file assertion above.
if grep -q "Exported $STILL_NAME" /tmp/still_status.txt; then
    check 0 "the status bar reports the exported image by name ($(tr -d '\n' < /tmp/still_status.txt | head -c 120))"
else
    check 1 "the status bar reports the exported image by name (got: $(tr -d '\n' < /tmp/still_status.txt 2>/dev/null | head -c 120))"
fi
grep -qF "$OUTDIR" /tmp/still_status.txt
check $? "and says it wrote into the throwaway directory, not the checkout"
assert_rendered "$SHOTS/still_01_form.png" "the export form for a still image"
assert_rendered "$SHOTS/still_02_done.png" "the app after the still export"

# ----------------------------------------------------------- a frame sequence
# The other image artifact, and the opposite set of questions: a frame range IS an
# input (ugoira caps it at 150 stills), a frame rate is not - the timing of a folder
# of numbered stills is authored on the upload page, and the destination says so
# (fps="ignored"), so the form must not ask for one.
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "'"$OUTDIR"'"
  '"$APP"' "'"$PROJ"'" >/tmp/probe_sequence.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  ae_press "File" "the File menu opens"
  ae_press "Export project as" "the export entry is there"
  require_export_flow
  ae set "Search destinations" "post.pixiv.ugoira" >/dev/null
  sleep 1.2
  ae_press "Use this destination" "the sequence destination is accepted"
  ae wait "Advanced" >/dev/null 2>&1 || true
  sleep 1.5
  # Shrink the frame so the stills render quickly: a sequence is a `free` size
  # destination, so its Width and Height belong to the user.
  ae set "Width" "160" >/dev/null; sleep 0.8
  ae set "Height" "256" >/dev/null; sleep 0.8
  # ...and a SHORT range, by name, with BOTH ends given so the count of files does not
  # depend on where the range of the project starts. The format caps the range at 150
  # frames and this probe is not here to render a hundred and fifty: what is asserted
  # is that the range the form carries is the range that reaches the disk, which 20
  # stills show as well as 150.
  ae set "First frame" "1" >/dev/null; sleep 0.6
  ae set "Last frame" "'"$SEQ_FRAMES"'" >/dev/null; sleep 0.6
  ae_report /tmp/seq_form.txt "Destination" "Image quality" "Width" "Height" "First frame" \
                               "Last frame" "Frame rate" "Format" "Quality"
  ae text > /tmp/seq_facts.txt 2>/dev/null
  import -window root '"$SHOTS"'/seq_01_form.png
  ae_press "Export" "the sequence form is answered"
  sleep 4
  xdotool key --clearmodifiers ctrl+a; sleep 0.4
  xdotool type --delay 25 "'"$SEQ_NAME"'"; sleep 0.6
  xdotool key Return
  # Wait for the RANGE to have reached the disk: the last frame is the file that proves
  # it, so waiting for the first file alone would report a partial sequence as done.
  for i in $(seq 1 240); do [ -s "'"$OUTDIR/$SEQBASE"'$(printf %04d '"$SEQ_FRAMES"').png" ] && break; sleep 0.5; done
  # ...and for the app to say it finished, which is the status bar naming the export.
  status=""
  for i in $(seq 1 30); do
    status=$(ae label "Exported '"$SEQ_NAME"'" 2>/dev/null)
    [ -n "$status" ] && break
  done
  printf "%s\n" "$status" > /tmp/seq_status.txt
  sleep 1
  import -window root '"$SHOTS"'/seq_02_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

assert_showing /tmp/seq_form.txt "Destination" "the sequence form states the destination it was given"
assert_showing /tmp/seq_form.txt "First frame" "a frame sequence keeps the frame range editable"
assert_showing /tmp/seq_form.txt "Last frame" "both ends of it"
assert_hidden /tmp/seq_form.txt "Frame rate" "and asks for no frame rate: a numbered sequence has no timing of its own"
assert_showing /tmp/seq_form.txt "Image quality" "a sequence offers the image writer's own quality control"
if grep -q "numbered PNG files" /tmp/seq_facts.txt; then
    check 0 "the facts state the artifact as numbered files (Artifact: N numbered PNG files)"
else
    check 1 "the facts state the artifact as numbered files (got: $(tr '\n' ' ' < /tmp/seq_facts.txt 2>/dev/null | head -c 200))"
fi

# One file per frame, numbered from 0001, at the name the user typed.
count=$(ls "$OUTDIR/$SEQBASE"[0-9]*.png 2>/dev/null | wc -l)
[ "$count" = "$SEQ_FRAMES" ]
check $? "the sequence wrote one file per frame of the range it was given ($count of $SEQ_FRAMES)"
assert_exists "$OUTDIR/${SEQBASE}0001.png" "the first frame is numbered from 0001"
assert_exists "$OUTDIR/${SEQBASE}$(printf %04d "$SEQ_FRAMES").png" "the last frame of the range is there"
total=$(du -cb "$OUTDIR/$SEQBASE"[0-9]*.png 2>/dev/null | tail -1 | cut -f1)
[ "${total:-0}" -le 31457280 ] && [ "${total:-0}" -gt 0 ]
check $? "the whole set is inside ugoira's 30 MB cap (${total:-0} bytes)"
# The app's own verdict on the sequence export.
if grep -q "Exported $SEQ_NAME" /tmp/seq_status.txt; then
    check 0 "the status bar reports the sequence export by name ($(tr -d '\n' < /tmp/seq_status.txt | head -c 120))"
else
    check 1 "the status bar reports the sequence export by name (got: $(tr -d '\n' < /tmp/seq_status.txt 2>/dev/null | head -c 120))"
fi
grep -qF "$OUTDIR" /tmp/seq_status.txt
check $? "and says it wrote the frames into the throwaway directory, not the checkout"
assert_rendered "$SHOTS/seq_01_form.png" "the export form for a frame sequence"
assert_rendered "$SHOTS/seq_02_done.png" "the app after the sequence export"

summary probe_still
