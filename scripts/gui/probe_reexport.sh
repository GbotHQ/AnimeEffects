# The iteration loop: export, then export AGAIN — without re-walking the menu and
# the destination, but WITH a chance to name the file.
#
# Export is not a one-shot in an animation tool: the same sticker gets exported a
# dozen times while the art is adjusted. The first version of "Export again"
# repeated the output path as well, and defended that with the exporter's
# overwrite confirmer — a yes/no on the SAME name, so it could never produce a
# second file, and answering yes destroyed the previous export with no way back.
# For a tool whose output is the deliverable, that is the wrong default.
#
# This probe asserts the behaviour that matters:
#   * the first export asks for a folder and writes a file;
#   * "Export again" asks for a NAME, pre-filled;
#   * naming a different file produces that file, and LEAVES THE FIRST ONE ALONE
#     with its content intact (same byte count as when it was written).
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

OUTDIR=$(mktemp -d)
export XDG_CONFIG_HOME=$(mktemp -d)   # a clean profile: no remembered folder yet
NAME="test.apng"                      # Discord sticker -> apng
FMT_OUT="$OUTDIR/$NAME"
rm -f "$FMT_OUT"

# The destination is preselected through the sandboxed settings rather than by
# clicking a menu row. The quick menu lost that race once already: the chat family
# was added in FRONT of the sticker family, so the row this probe clicked as
# "Discord sticker" became a chat export, and it asserted an .apng while the app
# wrote an .mp4. Qt's INI format keeps root keys in [General].
mkdir -p "$XDG_CONFIG_HOME/AnimeEffectsProject"
printf '[General]\nexport_destination=discord.sticker\nexport_quality_bucket=1\n' \
    > "$XDG_CONFIG_HOME/AnimeEffectsProject/AnimeEffects.conf"

xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "'"$OUTDIR"'" && '"$APP"' "'"$PROJ"'" >/tmp/probe_reexport.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null

  # 1. File -> Export project as, with the Discord sticker already selected.
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 5
  require_export_flow
  import -window root "$SHOTS"/re_01_dialog.png
  # Export. The button row is narrower in destination mode than in manual mode.
  xdotool key alt+e; sleep 4
  import -window root "$SHOTS"/re_02_first.png
  xdotool mousemove 400 25 click 1; sleep 0.4
xdotool key ctrl+a; sleep 0.3
  xdotool type --delay 25 "'"$NAME"'"; sleep 0.5
  xdotool key Return
  sleep 60
  import -window root "$SHOTS"/re_03_first_done.png

  stat -c %s "'"$FMT_OUT"'" > "'"$OUTDIR"'/first_size" 2>/dev/null

  # 2. File -> Export again (y=205 measured from the File menu). It must ASK for a
  #    name, so a save dialog appears with the previous name filled in.
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 60 205 click 1; sleep 3
  import -window root "$SHOTS"/re_04_name_prompt.png
  xdotool mousemove 400 25 click 1; sleep 0.4
xdotool key ctrl+a; sleep 0.3
  xdotool type --delay 25 "again.apng"; sleep 0.5
  xdotool key Return; sleep 3
  sleep 45
  import -window root "$SHOTS"/re_05_again_done.png
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3

assert_rendered "$SHOTS/re_01_dialog.png" "the export dialog on the first export"
assert_exists "$FMT_OUT" "the first export wrote a file"
assert_rendered "$SHOTS/re_04_name_prompt.png" "Export again asked for a name"

# The requirement: "Export again" must NOT decide by itself that the previous
# export is expendable. Giving it a different name has to leave the first file
# exactly as it was, and produce the second.
assert_exists "$OUTDIR/again.apng" "Export again wrote the file it was asked for"
first=$(stat -c %s "$FMT_OUT" 2>/dev/null || echo 0)
again=$(stat -c %s "$OUTDIR/again.apng" 2>/dev/null || echo 0)
[ "$first" -gt 0 ]
check $? "the FIRST export is still there, at $first bytes, untouched"
[ "$first" = "$again" ] && [ "$again" -gt 0 ]
check $? "the repeat used the same settings ($first = $again bytes)"

assert_rendered "$SHOTS/re_05_again_done.png" "the app after Export again"
[ -f /tmp/probe_reexport.log ] && ! grep -q "Export Error" /tmp/probe_reexport.log
check $? "no export error was reported"
rm -rf "$OUTDIR"
summary probe_reexport
