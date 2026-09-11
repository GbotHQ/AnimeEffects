#!/bin/bash
# See _lib.sh: these drive the real app under Xvfb because the wiring they check
# is not reachable from a unit test.
#
# Byte caps are hard upload rejections, not preferences. Two of these cases are
# the ones that were broken:
#
#   * The cap was enforced on the video path only. The dialog stated Telegram's
#     256 KB, WeChat's 500 KB and Twitch's 1 MB while the GIF branch returned
#     before the fit loop, so a GIF went out at whatever size it came to.
#   * The fit loop left the LAST PROBE's file on disk. A bisection stops on the
#     probe that did not fit, so the app could report "fitted at 119199 bytes"
#     while the file was the 120687-byte probe, over the cap it claimed to meet.
#
# The destination is preselected through QSettings rather than by clicking a menu
# row. The quick menu is grouped by family and the chat family was added in front
# of the sticker family, which silently turned this probe's "Telegram sticker"
# click into a chat export: it asserted test.webm while the app wrote test.mp4.
# An index into a reordered list is not a destination.
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/_lib.sh" "$@"

OUTDIR=${2:-/tmp}
# Sandboxed settings: the destination is an INPUT to this probe, and writing it
# into the developer's real config would both depend on and clobber their state.
CFG=$(mktemp -d)
export XDG_CONFIG_HOME="$CFG"
mkdir -p "$CFG/AnimeEffectsProject"
rm -f "$OUTDIR/test.webm" "$OUTDIR/test.gif"

# run_case <destination id> <output name> <cap bytes> <what>
run_case() {
    local dest="$1" name="$2" cap="$3" label="$4"
    # Qt's INI format puts root-level keys in a [General] section, and a key
    # written above any group header is not read back as one: the first attempt at
    # this preselected nothing and the dialog came up in manual mode.
    printf '[General]\nexport_destination=%s\nexport_quality_bucket=1\n' "$dest" \
        > "$CFG/AnimeEffectsProject/AnimeEffects.conf"
    xvfb-run -a --server-args="-screen 0 1440x900x24" bash -c '
      cd '"$OUTDIR"' && '"$APP"' '"$PROJ"' >/tmp/probe_capped_'"$dest"'.log 2>&1 &
      app=$!
      sleep 14
      W=$(xdotool search --name "AnimeEffects" | tail -1)
      xdotool windowactivate --sync $W 2>/dev/null
      xdotool mousemove 16 10 click 1; sleep 1.2
      xdotool mousemove 81 162 click 1; sleep 5
      import -window root '"$SHOTS"'/cap_'"$dest"'_01_dialog.png
      # Export, then accept the name the OS dialog opens with. The button row is
      # narrower in destination mode than in manual mode, where (399,676) is Export:
      # here it is CANCEL, which closes the dialog and writes nothing.
      xdotool mousemove 210 676 click 1; sleep 4
      xdotool key ctrl+a; sleep 0.3
      xdotool type --delay 25 '"$name"'; sleep 0.5
      xdotool key Return
      sleep 240
      import -window root '"$SHOTS"'/cap_'"$dest"'_02_done.png
      kill $app 2>/dev/null; wait $app 2>/dev/null
    ' 2>&1 | tail -2
    assert_under "$OUTDIR/$name" "$cap" "$label"
    assert_rendered "$SHOTS/cap_${dest}_02_done.png" "the app after the $dest export"
}

# A video destination with a cap: the case that already worked.
run_case telegram.sticker test.webm 262144 \
    "the Telegram sticker is inside its 256 KB cap"

# A GIF destination with a cap: media::GifEncoder owns the artifact, and the fit
# loop used to know only about the video encoder.
run_case wechat.sticker test.gif 512000 \
    "the WeChat sticker GIF is inside its 500 KB cap"

rm -rf "$CFG"
summary probe_capped
