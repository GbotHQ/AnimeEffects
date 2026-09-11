#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  '"$APP"' "'"$PROJ"'" >/tmp/probe_app.log 2>&1 &
  app=$!
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 3
  # open the destination combo (now the first row of the dialog)
  xdotool mousemove 658 61 click 1; sleep 1.5
  import -window root '"$SHOTS"'/dst_01_list.png
  # walk to item 2 (Telegram custom emoji) and commit
  xdotool key Down; sleep 0.3; xdotool key Down; sleep 0.3; xdotool key Return; sleep 2
  import -window root '"$SHOTS"'/dst_02_chosen.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
ls -1 "$SHOTS" | grep dst
assert_rendered "$SHOTS/dst_02_chosen.png" "the dialog after choosing a destination"
summary probe_dest
assert_rendered "$SHOTS/dst_02_chosen.png" "the dialog after choosing a destination"
summary probe_dest
