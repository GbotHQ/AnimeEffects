#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
# Drive a real export through the new dialog: pick an artifact, press Export,
# name a file in the OS dialog, and leave the resulting file on disk.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

FMT_INDEX=${2:-0}
DEST_INDEX=${4:-}
OUT=${3:-/tmp/ae_probe_out}
# The file dialog starts in the app's working directory and treats what is typed
# as a name IN it, so the app is launched from the target directory and only the
# basename is typed — typing an absolute path there makes Qt resolve
# "<cwd>//tmp/name" and the export is (correctly) refused for a missing directory.
OUTDIR=$(dirname "$OUT")
OUTNAME=$(basename "$OUT")
# Remove the target first. This probe asserts `ls "$OUT"*` is non-empty, and it was
# passing on a file from an earlier run — a gate that tested nothing while reading
# as coverage. The assertion has to fail when the export does not write.
rm -f "$OUT"*
xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
  cd "'"$OUTDIR"'" && '"$APP"' "'"$PROJ"'" >/tmp/probe_export.log 2>&1 &
  app=$!
  sleep 14
  W=$(xdotool search --name "AnimeEffects" | tail -1)
  xdotool windowactivate --sync $W 2>/dev/null
  xdotool mousemove 16 10 click 1; sleep 1.2
  xdotool mousemove 81 162 click 1; sleep 5
  # optionally choose a destination first (its plan overrides the artifact)
  if [ -n "'"$DEST_INDEX"'" ]; then
    xdotool mousemove 658 61 click 1; sleep 1.0
    xdotool key Home; sleep 0.3
    for i in $(seq 1 '"$DEST_INDEX"'); do xdotool key Down; sleep 0.2; done
    xdotool key Return; sleep 2
  fi
  # choose the artifact (Format row)
  xdotool mousemove 658 337 click 1; sleep 1.0
  xdotool key Home; sleep 0.2
  for i in $(seq 1 '"$FMT_INDEX"'); do xdotool key Down; sleep 0.15; done
  xdotool key Return; sleep 1.5
  # click a neutral spot so focus is back on the dialog, then the Export button
  xdotool mousemove 700 200 click 1; sleep 1.0
  import -window root '"$SHOTS"'/exp_01_dialog.png
  xdotool mousemove 399 676 click 1; sleep 4
  import -window root '"$SHOTS"'/exp_02_filediag.png
  # type the target path into the file dialog and accept
  xdotool key ctrl+a; sleep 0.3
  xdotool type --delay 25 "'"$OUTNAME"'"; sleep 0.5
  xdotool key Return; sleep 25
  import -window root '"$SHOTS"'/exp_03_after.png
  # if the validator objected, open its details so the reason is visible
  xdotool mousemove 393 270 click 1; sleep 2
  import -window root '"$SHOTS"'/exp_04_details.png
  xdotool mousemove 520 455 click 1; sleep 1
  kill $app 2>/dev/null; wait $app 2>/dev/null
' 2>&1 | tail -3
# The typed name has no extension, so the artifact's is appended ("ae_probe_out" ->
# "ae_probe_out.apng"). Assert on that BASE name rather than on "some file appeared"
# — a differently named output must not pass. Everything matching $OUT was deleted
# before the run, so whatever is here now was written by this run.
f=$(ls "$OUT"* 2>/dev/null | head -1)
case "$f" in
    "$OUT".*) check 0 "the exported file is named from the typed base name ($f)";;
    *) check 1 "the exported file is named from the typed base name (got '$f')";;
esac
[ -n "$f" ] && assert_exists "$f" "the exported file"
assert_rendered "$SHOTS/exp_03_after.png" "the app after the export"
summary probe_export
