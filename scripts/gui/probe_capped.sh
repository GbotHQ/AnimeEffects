#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
# Quick export a capped destination from the real app: Telegram stickers are
# rejected above 256 KB, so the file must land under that.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

OUTDIR=${2:-/tmp}
rm -f "$OUTDIR/test.webm" "$OUTDIR/test.png" "$OUTDIR/test.gif" "$OUTDIR/test.apng" "$OUTDIR/test.webp" "$OUTDIR/test.mov" "$OUTDIR/test.mkv" "$OUTDIR/test.mp4"
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "'"$OUTDIR"'" && '"$APP"' "'"$PROJ"'" >/tmp/probe_capped.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  # File -> Quickly export as -> Telegram sticker
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 60 182; sleep 1.5
  xdotool mousemove 300 182 click 1; sleep 3
  import -window root "$SHOTS"/cap_01_folder.png
  # the folder chooser is already in the target directory; accept it
  xdotool key Return; sleep 3
  import -window root "$SHOTS"/cap_02_after.png
  sleep 300
  import -window root "$SHOTS"/cap_03_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
assert_under "$OUTDIR/test.webm" 262144 "the Telegram sticker is inside the platform cap"
assert_rendered "$SHOTS/cap_03_done.png" "the app after the capped export"
summary probe_capped
