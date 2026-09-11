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
  pick() {  # $1 = index, $2 = shot name
    xdotool mousemove 658 61 click 1; sleep 1.0
    xdotool key Home; sleep 0.2
    for i in $(seq 1 "$1"); do xdotool key Down; sleep 0.15; done
    xdotool key Return; sleep 1.8
    import -window root '"$SHOTS"'/ax_"$2".png
  }
  pick 8  gif_twitch
  pick 12 website_alpha_choice
  pick 14 master
  pick 0  back_to_manual
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
ls -1 "$SHOTS" | grep ax_
assert_rendered "$SHOTS/ax_back_to_manual.png" "the dialog after the capability axes"
summary probe_axes
assert_rendered "$SHOTS/ax_back_to_manual.png" "the dialog after the capability axes"
summary probe_axes
