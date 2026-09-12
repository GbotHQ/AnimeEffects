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

# ---- driving the application by NAME -------------------------------------
#
# The probes used to click hardcoded pixels. A hardcoded pixel misses SILENTLY
# after a layout change: the click lands on a neighbour, the probe's screenshot is
# still not black, and it passes while testing the wrong widget - which is how ten
# probes kept "passing" after the export form was restructured (the Format row
# moved 70px and every one of them went on clicking the old spot). Everything below
# asks the widget where it is, or that it is there at all, so a layout change makes
# the probe fail loudly instead of quietly.
#
# The application must expose its accessibility tree, and the probe must say WHICH
# instance is its own: the developer's running app is on the same bus, and matching
# by name alone could drive that window instead. AE_UI_PID is set by the probe from
# the pid it launched.
AE_UI="$ROOT/scripts/gui/ae_ui.py"
# Exported for the same reason ROOT/APP/SHOTS are: the probes interpolate it inside
# the single-quoted `bash -c` string xvfb-run executes, and without the export the
# inner shell sees an empty path and every ae_* helper fails at once (37 assertions
# failing loudly is the good outcome; silently is what the old pixel clicks did).
export AE_UI
# Set by require_export_flow when a probe cannot reach the screen it asserts on.
export AE_STALE_FLAG="$(mktemp)"
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1

ae() { python3 "$AE_UI" "$@" 2>/dev/null; }

# The centre of a named widget, as "x y". Empty when the widget is not there.
ae_center() {
    local box
    box=$(ae extents "$1") || return 1
    [ -n "$box" ] || return 1
    echo $box | awk '{printf "%d %d", $1 + $3 / 2, $2 + $4 / 2}'
}

# Click a named widget, by asking it where it is.
ae_press() { # ae_press <name> <what-the-caller-expects-to-happen>
    local at
    at=$(ae_center "$1")
    if [ -z "$at" ]; then
        check 1 "$2 (no visible widget named '$1')"
        return 1
    fi
    xdotool mousemove $at click 1
    check 0 "$2"
}

# A probe whose navigation was recorded against the OLD export flow must not go on
# quietly clicking where widgets used to be: Export now opens the destination picker
# FIRST, so every probe that expected the parameter sheet at that point is driving
# something else, and its assertions (mostly "the screenshot is not black") pass
# anyway. Called right after the navigation prefix; exit 3 is STALE, which
# run_probes.sh reports as neither a pass nor a failure of the application.
# Exported as a FUNCTION, not just defined: the probes call it inside the
# single-quoted `bash -c` body xvfb-run runs, which is a new shell.
require_export_flow() {
    if ae wait "Use this destination" >/dev/null 2>&1; then
        return 0
    fi
    echo "STALE  navigation: the export flow did not open the destination picker." >&2
    echo "STALE  This probe's coordinates predate the destination-first flow." >&2
    echo "STALE  Re-record it against scripts/gui/ae_ui.py, which addresses widgets" >&2
    echo "STALE  by name instead of by pixel." >&2
    # The guard runs INSIDE the xvfb-run body, which is a child shell:
    # `exit 3` there ends that shell, not the probe. The flag is what the
    # outer script acts on: check() goes quiet and summary() reports STALE,
    # so a stale probe stops asserting instead of reporting failures of an
    # application it never reached.
    : > "$AE_STALE_FLAG"
    exit 3
}

export -f ae require_export_flow

# One line per name: "showing", "hidden" or "absent". The shell asserts on it.
ae_report() { # ae_report <file> <name>...
    local file=$1; shift
    ae report "$@" > "$file"
}

# The three states are three different assertions, and they mean different things:
# "showing" is what the user can act on, "hidden" is a control the artifact cannot
# use (the export form hides those rather than greying them out), and "absent" means
# the widget is not in this dialog at all.
assert_showing() { grep -qx "showing $2" "$1"; check $? "$3"; }
assert_hidden()  { grep -qx "hidden  $2" "$1"; check $? "$3"; }
assert_absent()  { grep -qx "absent  $2" "$1"; check $? "$3"; }

check() { # check <ok:0|1> <message>
    # A probe that never reached its screen asserts nothing: see require_export_flow.
    [ -n "${AE_STALE_FLAG:-}" ] && [ -f "$AE_STALE_FLAG" ] && return 0
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
    if [ -n "${AE_STALE_FLAG:-}" ] && [ -f "$AE_STALE_FLAG" ]; then
        rm -f "$AE_STALE_FLAG"
        echo "=== $1: STALE (its navigation no longer matches the application) ==="
        exit 3
    fi
    if [ "$FAILURES" -eq 0 ]; then
        echo "=== $1: OK ==="
    else
        echo "=== $1: $FAILURES FAILED ==="
    fi
    exit $((FAILURES > 0 ? 1 : 0))
}
