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
  AE_UI_PID=$app; export AE_UI_PID   # the window the tooling must address
  sleep 12
  xdotool search --name "AnimeEffects" | head -1
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 700 400 sleep 0.5
  # 1. open the File menu
  xdotool mousemove 16 10 click 1
  sleep 1.5
  import -window root '"$SHOTS"'/menu_01_file.png
  # 2. the quick-export entry. It opens the destination picker (a modal with a
  # search field) rather than a submenu tree, so this clicks it and captures what
  # comes up.
  xdotool mousemove 60 182 click 1
  sleep 3
  import -window root '"$SHOTS"'/menu_02_quick.png
  xdotool key Escape
  sleep 1.0
  import -window root '"$SHOTS"'/menu_03_sub.png
  sleep 0.5
  kill $app 2>/dev/null
  wait $app 2>/dev/null
' 2>&1 | tail -5
assert_rendered "$SHOTS/menu_02_quick.png" "the destination picker from the File menu"
summary probe_menu
