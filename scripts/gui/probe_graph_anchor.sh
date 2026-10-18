#!/bin/bash
# THE PANEL'S X IS A CAMERA, NOT A FRAME COUNTER.
#
# The graph panel's horizontal window is the timeline's, pushed as VALUES in FRACTIONAL frames, and
# the panel maps it into its own plot with a fractional rate. Two properties follow and are asserted
# here, on the application's own pixels - the only place where the mapping's INPUT
# (TimeLineWidget's exact viewport) and its OUTPUT (a drawn column) are both real: a middle-drag pan
# moves the content by exactly the pixels dragged, and a wheel notch holds the frame under the
# cursor.
#
# THE FEATURE IS THE PLAYHEAD, and no theme constant is baked in: its colour is learned from the
# pan pair below (the colour that tops exactly ONE column in both shots, at a different column in
# each - flat artwork and chrome top many columns at once, and grey is excluded outright), and it
# is the panel's only feature whose FRAME is known, which is what makes "did the camera hold the
# point under the cursor" a pixel question. ae_pix.py does the scan.
#
# THE PROJECT IS BUILT HERE, at the application's own default (600 frames): the pan assertion is
# only decisive where a frame is SEVERAL PIXELS wide, and that needs the timeline to have room to
# zoom - a short project clamps it, and the climbing notches then slide the anchored frame out of
# view instead of zooming (measured on a 160-frame project). A project of the caller's (arg 1 or
# AE_PROJECT) is used instead; the numbers in the log are the recording's.
#
# WHAT THIS PROBE DOES NOT COVER: the frame ladder itself. tests/time_scaler.cpp pins it directly -
# the x1.1 step at every index, the bounds, the inverse and the tick stride.
#
# FAILING LOUDLY is what the identification is for: a gesture that moves nothing prints none, and
# the run reports it instead of measuring a static artwork column.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

GEOM=/tmp/probe_graph_anchor_geom.txt
SCRATCH=/tmp/probe_graph_anchor
rm -f "$GEOM"
mkdir -p "$SCRATCH"
# The body runs in a child shell (see _lib.sh): names it uses directly rather than interpolated
# have to be exported, and SCRATCH is one of them (a helper in the body builds paths from it).
export SCRATCH

# The crop is inside the plot: past the value-label column on the left, BELOW the ruler band and
# the project tab bar (the ruler numbers move with the frame axis too, and a text column would
# compete with the playhead for "the colour that moved"), clear of the docks.
CROP_LEFT=8
CROP_TOP=52
CROP_RIGHT=8
CROP_BOTTOM=8
# How many rows one colour must cover for its column to be a candidate at all. The playhead
# covers all of them; the bar is well below that so an overlay of text cannot hide it.
MIN_COLOUR_FRACTION=0.35
# The pan is ONE pointer move of this many pixels, so the only rounding in the path is the
# timeline's own content-pixel step (a multi-step drag would add one rounding per event).
PAN_PIXELS=40
# Current-frame steps, for reading the panel's pixels-per-frame off the playhead's movement.
FRAME_STEPS=20
# The same at a high zoom, where a frame is several pixels: 20 frames would walk the playhead off
# the plot, and the rate is readable from a few just as well.
HIGH_FRAME_STEPS=5
# Frame-axis zoom-in notches for the high-zoom phase. The timeline's ladder tops out at 16 px/frame,
# so this reaches the ceiling from any start.
HIGH_ZOOM_NOTCHES=12

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "$AE_PROBE_DIR"
  "$APP" "${PROJ:-}" >/tmp/probe_graph_anchor_app.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID
  wait_ready 120 || sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # THE PROJECT, at the application own defaults (600 frames), so the timeline can be zoomed past
  # its floor without clamping.
  ae_press "File" "the File menu opens"
  sleep 1
  ae_press "New project" "the New project entry is pressed"
  sleep 2.5
  ae_activate "Create" "a default new project is created"
  sleep 4
  xdotool mousemove --sync 60 400; sleep 0.5

  read -r TX TY TW TH <<< "$(ae extents "Tools")"
  read -r PPX PPY PPW PPH <<< "$(ae extents "Properties")"
  read -r ANX ANY ANW ANH <<< "$(ae extents "Animation")"
  VL=$((TX + TW)); VT=$PPY; VR=$PPX; VB=$ANY
  CL=$((VL + '"$CROP_LEFT"')); CT=$((VT + '"$CROP_TOP"'))
  CW=$((VR - CL - '"$CROP_RIGHT"')); CH=$((VB - CT - '"$CROP_BOTTOM"'))
  MIDY=$((CT + CH / 2))
  PARKX=$((CL + CW / 8)); PARKY=$((CT + CH - 20))
  MINC=$(awk -v h=$CH -v f='"$MIN_COLOUR_FRACTION"' "BEGIN { printf \"%d\", h * f }")
  echo "VIEW=\"$VL $VT $VR $VB\"" >> '"$GEOM"'
  echo "CROP=\"$CL $CT $CW $CH\"" >> '"$GEOM"'

  park() { xdotool mousemove --sync $PARKX $PARKY; sleep 0.6; }
  shot() { import -window root "$1"; }
  raw() { magick "$1" -crop ${CW}x${CH}+${CL}+${CT} +repage -depth 8 rgb:"$2"; }
  pix() { python3 "$ROOT/scripts/gui/ae_pix.py" "$@"; }
  # The playhead column of a capture, given its learned colour; 99999 fails the assertion loudly
  # when the feature is not in view.
  col() { local c; c=$(pix line "$1" $CW $CH "$2" "$3" "$4" $MINC | cut -d" " -f1); case "$c" in ""|*[!0-9]*) echo 99999 ;; *) echo "$c" ;; esac; }

  park
  ae_press "Show animation graph" "the graph toggle is pressable (anchor probe)"
  sleep 1.5
  park

  # (1) THE PAN, which also names the playhead: a middle-drag of PAN_PIXELS must move the content
  # by exactly that many pixels. A whole-frame carrier snaps it to the nearest frame instead.
  shot "$SHOTS/anchor_01_pan_before.png"; raw "$SHOTS/anchor_01_pan_before.png" '"$SCRATCH"'/pa.raw
  xdotool mousemove --sync $((CL + CW / 2)) $MIDY
  sleep 0.3
  xdotool mousedown 2
  sleep 0.2
  xdotool mousemove --sync $((CL + CW / 2 + '"$PAN_PIXELS"')) $MIDY
  sleep 0.3
  xdotool mouseup 2
  sleep 1.0
  park
  shot "$SHOTS/anchor_02_pan_after.png"; raw "$SHOTS/anchor_02_pan_after.png" '"$SCRATCH"'/pb.raw
  MOVED=$(pix moved '"$SCRATCH"'/pa.raw '"$SCRATCH"'/pb.raw $CW $CH $MINC)
  echo "MOVED=\"$MOVED\"" >> '"$GEOM"'

  if [ "$MOVED" = "none" ]; then
      kill $app 2>/dev/null
      exit 0
  fi

  read -r PX0 PX1 PLR PLG PLB <<< "$MOVED"
  echo "PAN_X=\"$PX0 $PX1\"" >> '"$GEOM"'
  echo "COLOUR=\"$PLR $PLG $PLB\"" >> '"$GEOM"'

  # The panel own pixels per frame, from the playhead own frame: 20 current-frame steps.
  xdotool key --clearmodifiers --repeat '"$FRAME_STEPS"' --repeat-delay 60 ctrl+Right
  sleep 1.0
  park
  shot "$SHOTS/anchor_03_frameN.png"; raw "$SHOTS/anchor_03_frameN.png" '"$SCRATCH"'/fn.raw
  FX1=$(col '"$SCRATCH"'/fn.raw "$PLR" "$PLG" "$PLB")
  echo "FRAME_X=\"$PX1 $FX1\"" >> '"$GEOM"'

  # (2) THE ZOOM ANCHOR: notches with the pointer ON the playhead column, so the frame under the
  # cursor is the playhead own frame and an anchored zoom cannot move it. The first is a plain
  # notch (both axes), then the frame axis is climbed until a frame is several pixels wide - which
  # is the zoom the pan below has to survive.
  xdotool mousemove --sync $((CL + FX1)) $MIDY
  sleep 0.4
  shot "$SHOTS/anchor_04_zoom_before.png"; raw "$SHOTS/anchor_04_zoom_before.png" '"$SCRATCH"'/za.raw
  xdotool click 5
  sleep 1.0
  park
  shot "$SHOTS/anchor_05_zoom_after.png"; raw "$SHOTS/anchor_05_zoom_after.png" '"$SCRATCH"'/zb.raw
  ZX1=$(col '"$SCRATCH"'/zb.raw "$PLR" "$PLG" "$PLB")
  echo "ZOOM_X=\"$FX1 $ZX1\"" >> '"$GEOM"'
  echo "ZOOM_PIXELS=\"$(magick compare -metric AE "$SHOTS/anchor_04_zoom_before.png" "$SHOTS/anchor_05_zoom_after.png" null: 2>&1)\"" >> '"$GEOM"'

  xdotool mousemove --sync $((CL + ZX1)) $MIDY
  sleep 0.4
  xdotool click --repeat '"$HIGH_ZOOM_NOTCHES"' --delay 90 5
  sleep 1.2
  park
  shot "$SHOTS/anchor_06_high_before.png"; raw "$SHOTS/anchor_06_high_before.png" '"$SCRATCH"'/ha.raw
  HA=$(col '"$SCRATCH"'/ha.raw "$PLR" "$PLG" "$PLB")
  echo "HIGH_ANCHOR=\"$ZX1 $HA\"" >> '"$GEOM"'

  # ...and the playhead MOVES correctly at that zoom, which is the state the pan below is measured
  # in. It steps TOWARD the middle of the plot: a step the other way walks it off the edge at a high
  # zoom (measured: 5 frames at 14 px/frame from column 165 of a 194px plot, and a playhead outside
  # the plot is clipped and cannot be measured at all).
  STEPKEY=$( [ "$HA" -gt $((CW / 2)) ] && echo ctrl+Left || echo ctrl+Right )
  xdotool key --clearmodifiers --repeat '"$HIGH_FRAME_STEPS"' --repeat-delay 60 "$STEPKEY"
  sleep 1.0
  park
  shot "$SHOTS/anchor_07_high_frameN.png"; raw "$SHOTS/anchor_07_high_frameN.png" '"$SCRATCH"'/hn.raw
  HB=$(col '"$SCRATCH"'/hn.raw "$PLR" "$PLG" "$PLB")
  echo "HIGH_PPF=\"$HA $HB\"" >> '"$GEOM"'

  # (3) THE PAN AT THAT ZOOM: the same drag, with a frame now several pixels wide - a whole-frame
  # carrier misses by a frame, which at this zoom is a visible distance.
  xdotool mousemove --sync $((CL + HB)) $MIDY
  sleep 0.3
  xdotool mousedown 2
  sleep 0.2
  xdotool mousemove --sync $((CL + HB - '"$PAN_PIXELS"')) $MIDY
  sleep 0.3
  xdotool mouseup 2
  sleep 1.0
  park
  shot "$SHOTS/anchor_08_pan_zoomed_after.png"; raw "$SHOTS/anchor_08_pan_zoomed_after.png" '"$SCRATCH"'/pc.raw
  echo "PAN2_X=\"$HB $(col '"$SCRATCH"'/pc.raw "$PLR" "$PLG" "$PLB")\"" >> '"$GEOM"'

  kill $app 2>/dev/null
' || true

. "$GEOM" 2>/dev/null

if [ -z "${MOVED:-}" ] || [ "$MOVED" = "none" ]; then
    check 1 "the playhead is identified by a pan that moves it (the pan moved nothing, or the playhead is out of view)"
    summary probe_graph_anchor
fi

read -r PX0 PX1 <<< "$PAN_X"
read -r FX0 FX1 <<< "$FRAME_X"
read -r ZX0 ZX1 <<< "$ZOOM_X"
read -r HX0 HX1 <<< "${HIGH_ANCHOR:-0 0}"
read -r HP0 HP1 <<< "${HIGH_PPF:-0 0}"
read -r P20 P21 <<< "${PAN2_X:-0 0}"
pan=$((PX1 - PX0))
zoom=$((ZX1 - ZX0))
high_anchor=$((HX1 - HX0))
pan2=$((P20 - P21))
ppf=$(awk -v a="$FX1" -v b="$PX1" -v n="$FRAME_STEPS" 'BEGIN { printf "%.4f", (a - b) / n }')
zoomed_ppf=$(awk -v a="$ZX1" -v b="$FX1" 'BEGIN { printf "%.4f", a - b }')
# The frame steps run toward the middle of the plot, so the sign is the run's business: the rate is
# the movement over the step, in either direction.
high_ppf=$(awk -v a="$HP1" -v b="$HP0" -v n="$HIGH_FRAME_STEPS" 'BEGIN { printf "%.4f", (a > b ? a - b : b - a) / n }')
changed=${ZOOM_PIXELS%% *}
echo "measured: playhead rgb ${COLOUR:-?}, ppf $ppf px/frame, pan $pan px of $PAN_PIXELS, the notch moved it $zoom px ($changed px changed), high-zoom anchor $high_anchor px at $high_ppf px/frame, pan at that zoom $pan2 px"

check 0 "the playhead is found and moved by the pan (column $PX0 -> $PX1, colour ${COLOUR:-?})"
check "$([ "$pan" -gt $((PAN_PIXELS - 3)) ] && [ "$pan" -lt $((PAN_PIXELS + 3)) ] && echo 0 || echo 1)" \
    "a ${PAN_PIXELS}px middle-drag moves the content ${PAN_PIXELS}px, not a frame's worth (measured $pan px at $ppf px/frame)"
check "$([ "$zoom" -ge -1 ] && [ "$zoom" -le 1 ] && echo 0 || echo 1)" \
    "a wheel notch with the pointer ON a frame holds that frame still (measured $zoom px)"
check "$([ "${changed:-0}" -gt 500 ] && echo 0 || echo 1)" \
    "and the same notch really zoomed (${changed:-0} px changed)"
check "$([ "$high_anchor" -ge -1 ] && [ "$high_anchor" -le 1 ] && echo 0 || echo 1)" \
    "$HIGH_ZOOM_NOTCHES notches on the playhead hold it too (measured $high_anchor px)"
check "$(awk -v a="$ppf" -v b="$high_ppf" 'BEGIN { print (a > 0 && b > a * 3) ? 0 : 1 }')" \
    "and they climbed the zoom ($ppf -> $high_ppf px/frame)"
check "$([ "$pan2" -gt $((PAN_PIXELS - 2)) ] && [ "$pan2" -lt $((PAN_PIXELS + 2)) ] && echo 0 || echo 1)" \
    "at $high_ppf px/frame a frame is several pixels, and the same pan still moves exactly ${PAN_PIXELS}px (measured $pan2 px) - a whole-frame carrier cannot pass this one"
summary probe_graph_anchor
