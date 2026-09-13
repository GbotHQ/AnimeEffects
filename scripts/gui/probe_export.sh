#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# ONE REAL EXPORT, END TO END, THROUGH THE DESTINATION FLOW: File > Export project as,
# a destination chosen by name in the picker, the parameter form that destination
# dictates, the Export button, the platform file dialog, and the file that lands on
# disk.
#
# What this replaces. The probe used to click recorded pixels (mousemove 16 10, then
# 81 162 for the export entry, then 462 458 for the Format row) and to assert that the
# screenshot was not black. Export opens the destination picker FIRST now, so those
# coordinates belong to a flow that no longer exists: 81 162 is not the export entry
# any more, the click landed somewhere else, the screenshot was still not black, and
# the probe passed while never opening an export dialog at all. Every step below asks
# the widget where it is (ae_ui.py), and asserts something the application has to be
# right about.
#
# The BODY only navigates and leaves evidence. Every assertion runs out here, where the
# helpers and the failure count live.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"
# The revision of the export flow this probe's navigation was recorded against. See
# AE_FLOW_REVISION in _lib.sh: the guard refuses to run a probe that predates it.
export RECORDED_AGAINST=2

# THROWAWAY OUTPUT, never the checkout: the platform file dialog resolves a relative
# name against the application's working directory, so the app is launched from here
# and the name typed below lands here. The status message carries that absolute path,
# which is asserted below - the cheapest way to notice a probe writing into the repo.
OUTDIR="$AE_PROBE_DIR"
OUTNAME="probe_export.mp4"
OUT="$OUTDIR/$OUTNAME"
# The frame range this probe sets BY NAME. What the file has to match is the range the
# FORM ends up holding, which is read back inside the body and asserted below.
FIRST_FRAME=1
LAST_FRAME=24
rm -f "$OUT"
rm -f /tmp/exp_form.txt /tmp/exp_facts.txt /tmp/exp_fps.txt /tmp/exp_range.txt /tmp/exp_status.txt

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  # cd as its OWN command, not as `cd X && App &`. A background job whose body is a
  # compound command is a SUBSHELL, so $! names the subshell rather than the
  # application: AE_UI_PID then matches no accessible root, and every by-name helper
  # answers "no widget named ..." while a window is plainly on screen. That is what
  # the version this replaces did.
  cd "'"$OUTDIR"'"
  '"$APP"' "'"$PROJ"'" >/tmp/probe_export.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # 1. Export asks WHERE first. The menu and the entry are reached by name: nothing
  #    here depends on where a menu happens to be drawn.
  ae_press "File" "the File menu opens"
  ae_press "Export project as" "the export entry is there"
  require_export_flow
  # The ID, not the label: "A video for a chat app" is one of several rows whose
  # search haystack contains "video", and a query that matches several rows answers
  # with whichever the tree lists first. The id is unique, so the destination this
  # probe exports with is the one it names.
  ae set "Search destinations" "chat.generic.video" >/dev/null
  sleep 1.2
  ae_press "Use this destination" "the destination is accepted"
  # Wait for the plan to have been applied before snapshotting the form: a report
  # answers as soon as ANY name it was asked about is on screen, so it can describe
  # the form BEFORE the destination has filled it in.
  ae wait "Advanced" >/dev/null 2>&1 || true
  sleep 1.5

  # 2. A short range, set BY NAME: both ends of it, so the count the file should hold
  #    is the count this probe asked for. The project is 601 frames and a probe has
  #    minutes; that the range on the form is the range that reaches the file is shown
  #    by one second of animation as well as by twenty-five. (A destination that caps
  #    the DURATION decides the range itself, which is why probe_capped does not set
  #    it.)
  ae set "First frame" "'"$FIRST_FRAME"'" >/dev/null; sleep 0.6
  ae set "Last frame" "'"$LAST_FRAME"'" >/dev/null; sleep 0.6
  # Read the range BACK from the form. What the file has to match is the range the form
  # holds, not the one this probe typed: a spin box clamps to whatever the range of the
  # project allows, and a probe that assumed its own number stuck would report the
  # application as wrong for clamping it.
  printf "%s %s\n" \
      "$(ae value "First frame" 2>/dev/null | cut -d. -f1 | tr -dc 0-9)" \
      "$(ae value "Last frame" 2>/dev/null | cut -d. -f1 | tr -dc 0-9)" \
      > /tmp/exp_range.txt
  ae_report /tmp/exp_form.txt "Destination" "Width" "Height" "Frame rate" "First frame" "Last frame" "Image quality"
  # The frame rate the form will encode at, so the length of the file is judged
  # against the numbers the application reports rather than against what this probe
  # assumes they are.
  ae value "Frame rate" > /tmp/exp_fps.txt 2>/dev/null
  # What the form says it is about to make. The destination DICTATES the format, so
  # the form states it as a fact instead of offering a Format control, and the facts
  # are the data the destination declares, arriving through the plan.
  ae text > /tmp/exp_facts.txt 2>/dev/null
  import -window root '"$SHOTS"'/exp_01_form.png

  # 3. The form is answered, and the platform file dialog is TYPED INTO. That dialog
  #    is the one step that cannot be driven by name: it is a different toolkit in the
  #    same process, so its widgets are not in the accessibility tree of the
  #    application (ae_ui.py sees the main window tree). It opens with the name field
  #    focused and the suggested name selected, and a relative name resolves against
  #    the working directory of the application - which is why the app was launched
  #    from the output directory above.
  ae_press "Export" "the export form is answered"
  sleep 4
  xdotool key --clearmodifiers ctrl+a; sleep 0.4
  xdotool type --delay 25 "'"$OUTNAME"'"; sleep 0.6
  xdotool key Return

  # 4. The completion signal is what the APP says, not what is on disk: the muxer
  #    writes its header the moment the export opens, so a file that exists (48 bytes)
  #    is not a finished export, and that is exactly the file this probe used to
  #    accept. The status bar names the file it wrote when the export RETURNS; ae label
  #    finds it by name and prints the whole message.
  status=""
  for i in $(seq 1 60); do
    status=$(ae label "Exported '"$OUTNAME"'" 2>/dev/null)
    [ -n "$status" ] && break
  done
  echo "waited up to $((i * 10))s for the export to report"
  printf "%s\n" "$status" > /tmp/exp_status.txt
  sleep 1
  import -window root '"$SHOTS"'/exp_02_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

# The form the destination dictated: the destination itself, and the frames and rate
# it will be made of.
assert_showing /tmp/exp_form.txt "Destination" "the export form states the destination it was given"
assert_showing /tmp/exp_form.txt "Frame rate" "a video destination keeps the frame rate editable"
assert_showing /tmp/exp_form.txt "First frame" "and the frame range it will encode"
assert_showing /tmp/exp_form.txt "Last frame" "both ends of it"
# The other half of what a form asks: an image writer's quality control cannot affect a
# video, so it is hidden rather than greyed. (probe_still asserts the same control the
# other way round, on the artifact that does go through QImage::save().)
assert_hidden /tmp/exp_form.txt "Image quality" "and does not offer the image writer's quality control to a video"
# The chosen destination's own decisions, in the words on screen. "20 MB" is the cap
# this row declares in the destination table, so this checks that the destination's
# data reached the form rather than that some particular wording is used.
for want in "libx264 in mp4" "20 MB"; do
    if grep -q "$want" /tmp/exp_facts.txt; then
        check 0 "the facts state what this destination decides ($want)"
    else
        check 1 "the facts state what this destination decides ($want) (got: $(tr '\n' ' ' < /tmp/exp_facts.txt 2>/dev/null | head -c 200))"
    fi
done

# THE FILE. Non-empty is the floor; the frame range the form held is the assertion that
# the form this probe drove is the export that ran.
assert_exists "$OUT" "the export wrote the file the user named"
# The range the FORM held, as opposed to the one this probe typed into it: both bounds
# are read back above, and what the file must match is what the form said.
read -r first last < /tmp/exp_range.txt 2>/dev/null
[ -n "$first" ] && [ -n "$last" ] && [ "$first" -ge 0 ] 2>/dev/null && [ "$last" -gt "$first" ] 2>/dev/null
check $? "the form stated the frame range it would encode (${first:-none}..${last:-none})"
want=$(( ${last:-0} - ${first:-0} + 1 ))
# ...and that range is the SHORT one this probe set by name rather than the whole
# project: without this, a probe whose range input never arrived would still pass on a
# 601-frame export that took ten minutes to produce.
[ "$want" -ge 2 ] && [ "$want" -le 60 ]
check $? "and the range set by name is the range on the form ($want frames, not the project's 601)"
# The rate the form stated, so the length below is judged against the application's
# own numbers. An unreadable value is a failure, not a skipped assertion. The value
# interface reports a spin box as a floating-point number ("24.0"), so the integer
# test below needs a truncated copy of it.
fps=$(tr -dc '0-9.' < /tmp/exp_fps.txt 2>/dev/null)
fps_int=${fps%%.*}
[ -n "$fps_int" ] && [ "$fps_int" -gt 0 ] 2>/dev/null
check $? "the form stated the frame rate it would encode at (got '${fps:-none}')"
if [ -s "$OUT" ]; then
    frames=$(ffprobe -v error -select_streams v -show_entries stream=nb_frames -of csv=p=0 "$OUT" 2>/dev/null | head -1)
    [ -n "$frames" ] && [ "$frames" -gt 0 ] 2>/dev/null
    check $? "the export contains video frames (nb_frames=${frames:-none})"
    [ "${frames:-0}" = "$want" ]
    check $? "the frame range the form held is the range that was encoded ($want frames, got ${frames:-none})"
    duration=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT" 2>/dev/null | head -1)
    ok=$(awk -v d="${duration:-0}" -v n="$want" -v f="${fps_int:-24}" \
         'BEGIN { sec = n / f; print (sec > 0 && d > sec * 0.7 && d < sec * 1.3) ? 0 : 1 }')
    check "$ok" "and the file lasts as long as those frames at that rate (${duration:-none} s for $want frames at ${fps_int:-?} fps)"
    size=$(stat -c %s "$OUT")
    [ "$size" -gt 1000 ]
    check $? "and it is a real file rather than a muxer header ($size bytes)"
fi

# What the app said when it finished. The status bar names the file, its size, and the
# directory it wrote to: a probe that exported into the checkout, or into a directory
# of its own choosing, fails here as well as at the file assertion above.
if grep -q "Exported $OUTNAME" /tmp/exp_status.txt; then
    check 0 "the status bar reports the export by name ($(tr -d '\n' < /tmp/exp_status.txt | head -c 120))"
else
    check 1 "the status bar reports the export by name (got: $(tr -d '\n' < /tmp/exp_status.txt 2>/dev/null | head -c 120))"
fi
if grep -qF "$OUTDIR" /tmp/exp_status.txt; then
    check 0 "and says it wrote into the throwaway directory, not the checkout"
else
    check 1 "and says it wrote into the throwaway directory, not the checkout (got: $(tr -d '\n' < /tmp/exp_status.txt 2>/dev/null | head -c 120))"
fi
assert_rendered "$SHOTS/exp_01_form.png" "the export form carrying the chosen destination"
assert_rendered "$SHOTS/exp_02_done.png" "the app after the export"
summary probe_export
