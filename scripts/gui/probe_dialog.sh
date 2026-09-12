#!/bin/bash
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

rm -f "$SHOTS"/dlg_*.png
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  '"$APP"' "'"$PROJ"'" >/tmp/probe_app.log 2>&1 &
  app=$!
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 3
  require_export_flow
  import -window root '"$SHOTS"'/dlg_01.png
  sleep 0.3
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
ls -1 "$SHOTS" | grep dlg
assert_rendered "$SHOTS/dlg_01.png" "the export dialog"
summary probe_dialog
