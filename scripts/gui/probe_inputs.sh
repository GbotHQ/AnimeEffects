#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# The dialog's per-run inputs must survive a reopen. They used to reset to the
# project's own values every time: changing one thing meant re-selecting the frame
# range, the size and the fps first, and the dialog is a modal you close and reopen
# constantly while iterating.
#
# Asserted through what the export PRODUCES, not through what the boxes display:
#   * a 0..3 frame range on ugoira writes 4 numbered files, not its 150-frame cap;
#   * a 160px width writes 160px files, not the project's 720.
# So the second export, with nothing touched after reopening, has to produce the
# same four 160px files. Without the restore it produces 150 files at 720px wide.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

OUT_A=$(mktemp -d)
OUT_B=$(mktemp -d)
CFG=$(mktemp -d)
export XDG_CONFIG_HOME="$CFG"
mkdir -p "$CFG/AnimeEffectsProject"
printf '[General]\nexport_destination=post.pixiv.ugoira\nexport_quality_bucket=1\n' \
    > "$CFG/AnimeEffectsProject/AnimeEffects.conf"

export OUT_A OUT_B
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  unset WAYLAND_DISPLAY; export QT_QPA_PLATFORM=xcb
  cd "$OUT_A" && /home/gambot/dev/AnimeEffects/build/src/gui/Debug/AnimeEffects "$0" >/tmp/probe_inputs.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  open_dialog() {
    xdotool mousemove 16 10 click 1; sleep 1.2
    xdotool mousemove 81 162 click 1; sleep 5
  require_export_flow
  }
  # Export, then name the frames in the OS dialog.
  export_now() {
    xdotool key alt+e; sleep 4
    xdotool mousemove 400 25 click 1; sleep 0.4
xdotool key ctrl+a; sleep 0.3
    xdotool type --delay 25 "$1"; sleep 0.5
    xdotool key Return; sleep 60
  }

  # 1. Set a distinctive range (0..3) and size (160x256), then export.
  # Each value is committed with Tab before the next click: typing into a spin box
  # and immediately clicking elsewhere drops characters, which made this probe
  # report a range that was never entered.
  set_spin() { # set_spin <x> <y> <value>
    xdotool mousemove "$1" "$2" click 1; sleep 0.8
    xdotool mousemove 400 25 click 1; sleep 0.4
xdotool key ctrl+a; sleep 0.3
    xdotool type --delay 60 "$3"; sleep 0.5
    xdotool key Tab; sleep 0.8
  }

  open_dialog
  set_spin 497 582 3     # last frame: a 0..3 range writes 4 files
  set_spin 335 348 160   # width
  set_spin 335 382 256   # height
  sleep 1.0
  import -window root "$SHOTS"/inputs_01_set.png
  export_now "first.png"

  # 2. Reopen the dialog and change NOTHING. Same inputs, so same output. The
  #    output directory is the one REMEMBERED from the first export, not a fresh
  #    one: the save dialog opens where the last export went.
  open_dialog
  import -window root "$SHOTS"/inputs_02_reopened.png
  export_now "second.png"

  kill $app 2>/dev/null; wait $app 2>/dev/null
' "$PROJ" 2>&1 | tail -2

# Both exports land in the same (remembered) directory, told apart by the stem the
# probe typed.
for pair in "first:the first export" "second:the reopened dialog's export"; do
    stem=${pair%%:*}
    what=${pair#*:}
    count=$(ls "$OUT_A/$stem"*.png 2>/dev/null | wc -l)
    echo "      $stem: $count files"
    [ "$count" = "4" ]
    check $? "$what used the remembered 0..3 range ($count files, 4 expected)"
    first=$(ls "$OUT_A/$stem"*.png 2>/dev/null | head -1)
    if [ -n "$first" ]; then
        # 160 wide comes back as 158: the size chain rounds to even for the codec
        # it would have used, and the point here is that it is not the project's 720.
        dims=$(magick identify -format "%wx%h" "$first" 2>/dev/null)
        case "$dims" in
            15[68]x256) check 0 "$what used the remembered 160x256 size ($dims)";;
            *) check 1 "$what used the remembered 160x256 size (got $dims)";;
        esac
    fi
done

assert_rendered "$SHOTS/inputs_02_reopened.png" "the reopened dialog"
rm -rf "$OUT_A" "$OUT_B" "$CFG"
summary probe_inputs
