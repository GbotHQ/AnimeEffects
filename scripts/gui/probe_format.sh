#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# This probe is about the MANUAL ESCAPE HATCH: the mode a user gets when no
# destination in the table describes what they are making, where the container
# and the codec are their own decision. It checks the two things that mode has to
# get right - the Format row is really there, and the Codec row answers about the
# container that was just chosen rather than about the one before it - and then
# that a still-image format is honest about itself: a PNG sequence offers Image
# quality (the writer setting it actually uses) and withdraws the frame rate (a
# directory of stills has no timing).
#
# The previous version of this probe drove the Format row by pixel and asserted
# that a screenshot was not black, so it passed while the export flow was
# restructured underneath it. Everything here addresses widgets BY NAME: a combo
# item is chosen by clicking the ITEM the accessibility tree names, so the item
# order in the popup is not recorded anywhere either.
#
# The BODY only navigates and leaves evidence. The assertions run out here.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"
# The revision of the export flow this probe's navigation was recorded against. See
# AE_FLOW_REVISION in _lib.sh: the guard refuses to run a probe that predates it.
export RECORDED_AGAINST=2

rm -f /tmp/fmt_manual.txt /tmp/fmt_webm_codec.txt /tmp/fmt_webm_facts.txt \
      /tmp/fmt_mp4_format.txt /tmp/fmt_mp4_codec.txt /tmp/fmt_mp4_facts.txt \
      /tmp/fmt_seq.txt /tmp/fmt_seq_format.txt

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  '"$APP"' "'"$PROJ"'" >/tmp/probe_app.log 2>&1 &
  app=$!
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # 1. Export asks WHERE first. The escape hatch is one of the answers the picker
  #    offers, and it is the answer this probe is about.
  ae_press "File" "the File menu opens"
  ae_press "Export project as" "the export entry is there"
  require_export_flow
  ae_press "My platform isn" "the picker offers the escape hatch and it is taken"
  ae wait "Format" >/dev/null 2>&1 || true
  sleep 2
  ae_report /tmp/fmt_manual.txt "Destination" "Format" "Codec" "Quality" \
      "Frame rate" "Image quality"
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/fmt_01_manual.png

  # Choosing a format is choosing the ITEM the popup names, not the Nth row: the
  # row under a name is the only thing a reordered list cannot silently change.
  choose() {  # $1 = combo to open, $2 = the item to pick, $3 = what that means
    ae_press "$1" "$1 opens"
    sleep 1.2
    ae_press "$2" "$3"
    sleep 1.8
  }

  # 2. WebM: the codec row must answer with a WebM codec.
  choose "Format" "WebM" "WebM is chosen in the Format row"
  ae value "Codec" > /tmp/fmt_webm_codec.txt 2>/dev/null
  ae text > /tmp/fmt_webm_facts.txt 2>/dev/null
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/fmt_02_webm.png

  # 3. MP4: a DIFFERENT container, so the codec row has to have changed with it.
  choose "Format" "MP4" "MP4 is chosen in the Format row"
  ae value "Format" > /tmp/fmt_mp4_format.txt 2>/dev/null
  ae value "Codec" > /tmp/fmt_mp4_codec.txt 2>/dev/null
  ae text > /tmp/fmt_mp4_facts.txt 2>/dev/null
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/fmt_03_mp4.png

  # 4. A still-image format: one PNG per frame, written by Qt image writer.
  choose "Format" "PNG sequence" "the PNG sequence format is chosen"
  ae value "Format" > /tmp/fmt_seq_format.txt 2>/dev/null
  ae_report /tmp/fmt_seq.txt "Format" "Codec" "Image quality" "Frame rate" "Quality" "Advanced"
  xdotool mousemove 700 600; sleep 0.5
  import -window root '"$SHOTS"'/fmt_04_png_sequence.png
  ae_press "Cancel" "the form closes on Cancel"
  sleep 1
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -4

# ---- the escape hatch states the artifact -------------------------------------
assert_showing /tmp/fmt_manual.txt "Format" "with no destination the Format row is the users decision"
assert_showing /tmp/fmt_manual.txt "Codec" "and the Codec row is there beside it"

# ---- the codec row answers about the container that was chosen ----------------
if grep -qx "VP9" /tmp/fmt_webm_codec.txt; then
    check 0 "WebM reports a WebM codec in the Codec row (VP9)"
else
    check 1 "WebM reports a WebM codec in the Codec row (got: $(tr -d '\n' < /tmp/fmt_webm_codec.txt 2>/dev/null))"
fi
if grep -qx "H.264" /tmp/fmt_mp4_codec.txt; then
    check 0 "choosing MP4 changes the Codec row to an MP4 codec (H.264)"
else
    check 1 "choosing MP4 changes the Codec row to an MP4 codec (got: $(tr -d '\n' < /tmp/fmt_mp4_codec.txt 2>/dev/null))"
fi
# The change is the point: a Codec row still reading the previous container codec
# would be the form answering a question about a format it is no longer making.
if grep -q "VP9\|VP8" /tmp/fmt_mp4_codec.txt; then
    check 1 "and it is no longer the WebM codec (got: $(tr -d '\n' < /tmp/fmt_mp4_codec.txt 2>/dev/null))"
else
    check 0 "and it is no longer the WebM codec"
fi
if grep -qx "MP4" /tmp/fmt_mp4_format.txt; then
    check 0 "and the Format row reports the container that was chosen"
else
    check 1 "and the Format row reports the container that was chosen (got: $(tr -d '\n' < /tmp/fmt_mp4_format.txt 2>/dev/null))"
fi
# The same pairing in the stated facts, which is where a user reads it.
for pair in "/tmp/fmt_webm_facts.txt:Codec: VP9 in WebM" "/tmp/fmt_mp4_facts.txt:Codec: H.264 in MP4"; do
    file=${pair%%:*}
    text=${pair#*:}
    if grep -qF "$text" "$file"; then
        check 0 "the facts state the container and codec together ($text)"
    else
        check 1 "the facts state the container and codec together ($text) (got: $(grep -i codec "$file" 2>/dev/null | tr '\n' ' ' | head -c 200))"
    fi
done

# ---- a still-image format is honest about what it can use --------------------
if grep -qx "PNG sequence" /tmp/fmt_seq_format.txt; then
    check 0 "the PNG sequence format is the one selected"
else
    check 1 "the PNG sequence format is the one selected (got: $(tr -d '\n' < /tmp/fmt_seq_format.txt 2>/dev/null))"
fi
assert_showing /tmp/fmt_seq.txt "Image quality" "a sequence of stills offers Image quality, the writer own setting"
assert_hidden /tmp/fmt_seq.txt "Frame rate" "and withdraws the frame rate, which a directory of stills has not got"
assert_hidden /tmp/fmt_seq.txt "Codec" "and the Codec row, since Qt writes these files rather than a video encoder"
assert_rendered "$SHOTS/fmt_01_manual.png" "the escape hatch"
assert_rendered "$SHOTS/fmt_04_png_sequence.png" "a still-image format on the same form"

summary probe_format
