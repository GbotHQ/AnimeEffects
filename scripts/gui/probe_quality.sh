#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
# Destination + the Lossless intent on a codec that cannot be lossless: the plan
# has to say so in the facts panel rather than quietly approximate.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  '"$APP"' "'"$PROJ"'" >/tmp/probe_qual.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 5
  require_export_flow
  # destination: Telegram sticker, through the picker (type, then Enter)
  xdotool mousemove 462 82 click 1; sleep 3
  xdotool type --delay 60 "telegram sticker"; sleep 1.2
  xdotool key Return; sleep 2
  import -window root '"$SHOTS"'/qual_01_balanced.png
  # quality: Lossless (0 Smaller, 1 Balanced, 2 Best, 3 Lossless)
  xdotool mousemove 462 138 click 1; sleep 1.0
  xdotool key Home; sleep 0.3
  for i in 1 2 3; do xdotool key Down; sleep 0.2; done
  xdotool key Return; sleep 2
  import -window root '"$SHOTS"'/qual_02_lossless.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
assert_rendered "$SHOTS/qual_02_lossless.png" "the Lossless-on-a-lost-codec facts"
summary probe_quality
