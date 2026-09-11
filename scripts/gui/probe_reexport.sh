#!/bin/bash
# The iteration loop: export, change nothing, export AGAIN — without re-walking
# the menu, the destination and the folder dialog.
#
# Export is not a one-shot in an animation tool. The same sticker gets exported
# a dozen times while the art is adjusted, and until now every one of those runs
# meant picking the destination again and navigating a folder dialog from scratch.
#
# Asserts: the first export prompts for a folder and writes a file; the second
# ("Export again") writes it again with NO prompt; and the file was actually
# rewritten (its modification time moves).
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

OUTDIR=$(mktemp -d)
export XDG_CONFIG_HOME=$(mktemp -d)   # a clean profile: no remembered folder yet
NAME="$(basename "${PROJ%.*}").apng"  # Discord sticker -> apng, named after the project
FMT_OUT="$OUTDIR/$NAME"
rm -f "$FMT_OUT"

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "'"$OUTDIR"'" && '"$APP"' "'"$PROJ"'" >/tmp/probe_reexport.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # 1. File -> Quickly export as -> Discord sticker (4th row: header, two Telegram, Discord)
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 60 182; sleep 1.2
  xdotool mousemove 300 257 click 1; sleep 3
  import -window root "$SHOTS"/re_01_folder_prompt.png
  xdotool key Return; sleep 3          # accept the folder the dialog opened in
  import -window root "$SHOTS"/re_02_first.png
  sleep 45
  import -window root "$SHOTS"/re_03_first_done.png

  # The file the second export has to recreate. Removing it is what makes the
  # check below able to FAIL: if "Export again" did nothing, or stopped to ask a
  # question nobody answers, the file stays gone.
  cp "'"$FMT_OUT"'" "'"$OUTDIR"'/first_copy.apng" 2>/dev/null
  rm -f "'"$FMT_OUT"'"

  # 2. File -> Export again, which must not ask for anything.
  #    y=205 measured from the File menu: New 36, Open 58, Recent 81, Save 115,
  #    Save as 138, Export project as 160, Quickly 182, AGAIN 205, Close 240.
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 60 205 click 1; sleep 3
  import -window root "$SHOTS"/re_04_again.png
  sleep 45
  import -window root "$SHOTS"/re_05_again_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3

assert_rendered "$SHOTS/re_01_folder_prompt.png" "the folder prompt on the first export"
assert_exists "$OUTDIR/first_copy.apng" "the first export wrote a file"
assert_exists "$FMT_OUT" "Export again recreated the file it was given no prompt for"

# Same destination, same settings: the re-export should reproduce the export.
first=$(stat -c %s "$OUTDIR/first_copy.apng" 2>/dev/null || echo 0)
second=$(stat -c %s "$FMT_OUT" 2>/dev/null || echo 0)
[ "$first" -gt 0 ] && [ "$first" = "$second" ]
check $? "the repeat is byte-count identical to the first export ($first vs $second)"

assert_rendered "$SHOTS/re_05_again_done.png" "the app after Export again"
[ -f /tmp/probe_reexport.log ] && ! grep -q "Export Error" /tmp/probe_reexport.log
check $? "no export error was reported"
rm -rf "$OUTDIR"
summary probe_reexport
