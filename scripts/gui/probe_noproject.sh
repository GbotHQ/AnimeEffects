#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  '"$APP"' >/tmp/probe_np.log 2>&1 &
  app=$!
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  # close the startup project, so the "no project" state is actually reached
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 60 253 click 1; sleep 2.5
  xdotool mousemove 700 400 sleep 0.5
  xdotool mousemove 16 10 click 1; sleep 1.5
  import -window root '"$SHOTS"'/np_file_menu.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
assert_rendered "$SHOTS/np_file_menu.png" "the File menu with no project"
summary probe_noproject
assert_rendered "$SHOTS/np_file_menu.png" "the File menu with no project"
summary probe_noproject
