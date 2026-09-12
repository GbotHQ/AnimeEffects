#!/bin/bash
# Shared setup for the GUI probes.
#
# These probes drive the real application under Xvfb with xdotool. They exist
# because the parts that keep breaking are the parts no unit test can reach: the
# export dialog's wiring, the quick-export menu, the file dialog, and whether a
# long note is actually visible. Every bug they found was invisible to the suite:
# a remembered palette blocking every export, a Quality control wired to nothing,
# size caps stated but never applied.
#
# Sourced, not executed. Callers get:
#   $ROOT   repository root (derived, so this works from any checkout)
#   $APP    the Debug application binary
#   $SHOTS  where screenshots are written (gitignored; override with AE_SHOTS)
#   $PROJ   a project to open (arg 1, or the first .anie found, or AE_PROJECT)
# and the helpers below, each of which FAILS LOUDLY rather than returning quietly.

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
APP="$ROOT/build/src/gui/Debug/AnimeEffects"
SHOTS="${AE_SHOTS:-$ROOT/build/shots}"
FAILURES=0

mkdir -p "$SHOTS"

# Exported because the probes interpolate these INSIDE the single-quoted
# `bash -c` string that xvfb-run executes: without this the inner shell sees an
# empty $SHOTS and writes screenshots to "/" instead.
export ROOT APP SHOTS PROJ

if [ ! -x "$APP" ]; then
    echo "FAIL  no application binary at $APP (build the Debug target first)"
    exit 2
fi

PROJ="${1:-${AE_PROJECT:-}}"
if [ -z "$PROJ" ]; then
    PROJ=$(find "$HOME/Downloads" "$ROOT" -maxdepth 3 -name '*.anie' 2>/dev/null | head -1)
fi
if [ -z "$PROJ" ] || [ ! -f "$PROJ" ]; then
    echo "FAIL  no project file: pass one as arg 1, or set AE_PROJECT"
    exit 2
fi

# Xvfb + the xcb platform: the GL app cannot run offscreen, and Qt6 prefers
# Wayland when WAYLAND_DISPLAY is set, which yields black captures.
unset WAYLAND_DISPLAY
export QT_QPA_PLATFORM=xcb

# SANDBOX THE SETTINGS. A probe drives the real application, and the application
# writes QSettings on every export: the remembered destination, the last output
# directory and name, the per-destination folders, the remembered frame range and
# size. Without this the probes edited the DEVELOPER'S OWN settings -- they set
# export_destination to whatever the probe was testing and pointed the next real
# export at /tmp -- which is both a rude thing to do and a way to make a passing
# probe depend on state a previous probe left behind. One sandbox here covers every
# probe, because they all source this file.
if [ -z "${AE_KEEP_CONFIG:-}" ]; then
    export XDG_CONFIG_HOME="$(mktemp -d)"
fi
# Probes that export also need a directory that is NOT the repository: the OS file
# dialogs default to the process working directory, and a probe that forgets to
# change it writes its test output into the checkout.
export AE_PROBE_DIR="$(mktemp -d)"

check() { # check <ok:0|1> <message>
    if [ "$1" -eq 0 ]; then
        echo "PASS  $2"
    else
        echo "FAIL  $2"
        FAILURES=$((FAILURES + 1))
    fi
}

# A capture that is entirely black means the app never rendered (crashed, or the
# window was never mapped). This is the cheapest possible smoke assertion and it
# caught a genuine crash during this work.
assert_rendered() { # assert_rendered <png> <what>
    local mean
    mean=$(magick "$1" -format "%[fx:mean]" info: 2>/dev/null || echo "0")
    local ok
    ok=$(awk -v m="$mean" 'BEGIN { print (m > 0.001) ? 0 : 1 }')
    check "$ok" "$2 rendered (mean brightness $mean, 0 would be a black frame)"
}

assert_exists() { # assert_exists <path> <what>
    [ -s "$1" ]; check $? "$2 exists and is non-empty ($(stat -c %s "$1" 2>/dev/null || echo 0) bytes)"
}

assert_under() { # assert_under <path> <max bytes> <what>
    local size
    size=$(stat -c %s "$1" 2>/dev/null || echo -1)
    [ "$size" -gt 0 ] && [ "$size" -le "$2" ]
    check $? "$3 ($size <= $2 bytes)"
}

summary() { # summary <probe name>
    if [ "$FAILURES" -eq 0 ]; then
        echo "=== $1: OK ==="
    else
        echo "=== $1: $FAILURES FAILED ==="
    fi
    exit $((FAILURES > 0 ? 1 : 0))
}
