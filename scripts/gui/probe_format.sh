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
  require_export_flow
  pick() {  # $1 = index, $2 = name
    xdotool mousemove 658 337 click 1; sleep 1.0
    xdotool key Home; sleep 0.2
    for i in $(seq 1 "$1"); do xdotool key Down; sleep 0.15; done
    xdotool key Return; sleep 1.8
    import -window root '"$SHOTS"'/fmt_"$2".png
  }
  pick 2  gif
  pick 9  png_seq
  pick 5  mov
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
assert_rendered "$SHOTS/fmt_png_seq.png" "the Format escape hatch"
summary probe_format
assert_rendered "$SHOTS/fmt_png_seq.png" "the Format escape hatch"
summary probe_format
