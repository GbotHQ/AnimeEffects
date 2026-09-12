#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "$AE_PROBE_DIR" && '"$APP"' "'"$PROJ"'" >/tmp/probe_app.log 2>&1 &
  app=$!
  sleep 12
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 3
  # $1 = what to type in the picker search field, $2 = shot name. Typing a name is
  # how a person chooses a destination now, so it is how the probe does it: no index
  # into a list that reorders whenever the table gains a row. (No apostrophes here:
  # this whole block is inside a single-quoted bash -c argument.)
  pick() {
    xdotool mousemove 462 82 click 1; sleep 3
    xdotool key ctrl+a; sleep 0.2
    xdotool type --delay 60 "$1"; sleep 1.2
    xdotool key Return; sleep 2
    import -window root '"$SHOTS"'/ax_"$2".png
  }
  pick "twitch"  gif_twitch
  pick "website" website_alpha_choice
  pick "master"  master
  pick 0  back_to_manual
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
ls -1 "$SHOTS" | grep ax_
assert_rendered "$SHOTS/ax_back_to_manual.png" "the dialog after the capability axes"
summary probe_axes
assert_rendered "$SHOTS/ax_back_to_manual.png" "the dialog after the capability axes"
summary probe_axes
