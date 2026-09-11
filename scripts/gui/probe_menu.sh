#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
# Open the File menu and the "Quickly export as" submenu, screenshotting each step.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

export -n WAYLAND_DISPLAY 2>/dev/null || unset WAYLAND_DISPLAY

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  '"$APP"' "'"$PROJ"'" >/tmp/probe_app.log 2>&1 &
  app=$!
  sleep 12
  xdotool search --name "AnimeEffects" | head -1
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 700 400 sleep 0.5
  # 1. open the File menu
  xdotool mousemove 16 10 click 1
  sleep 1.5
  import -window root '"$SHOTS"'/menu_01_file.png
  # 2. hover the quick-export entry so its submenu opens
  xdotool mousemove 60 182
  sleep 1.5
  import -window root '"$SHOTS"'/menu_02_quick.png
  # 3. walk down the submenu entries
  xdotool mousemove 300 190
  sleep 1.0
  import -window root '"$SHOTS"'/menu_03_sub.png
  sleep 0.5
  kill $app 2>/dev/null
  wait $app 2>/dev/null
' 2>&1 | tail -5
ls -1 "$SHOTS"
assert_rendered "$SHOTS/menu_03_sub.png" "the quick-export submenu"
summary probe_menu
assert_rendered "$SHOTS/menu_03_sub.png" "the quick-export submenu"
summary probe_menu
