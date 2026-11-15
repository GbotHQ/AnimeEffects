#!/usr/bin/env bash
# Builds and runs the property-panel widget test (tests/panel_blendif.cpp), which drives the
# real gui::prop::ConstantPanel - the widget the property dock builds for a selected node - and
# checks its Photoshop "Blend If" surface.
#
#   AE_BUILD_DIR   build tree holding the binary (default: build)
#   AE_QT_PLATFORM Qt platform plugin to try first (default: offscreen; the test falls back to
#                  the real display when the offscreen plugin cannot provide a GL context)
#
# The test needs an OpenGL 4.0 core context because building a LayerNode compiles its mesh
# transform shader. Exit codes: 0 = all checks passed, 1 = a check failed, 2 = no usable GL
# context, 3 = build failure.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${AE_BUILD_DIR:-build}"
BIN="$BUILD_DIR/tests/Debug/panel_blendif"

echo "--- building panel_blendif ($BUILD_DIR) ---"
cmake --build "$BUILD_DIR" --target panel_blendif --config Debug || exit 3
if [[ ! -x "$BIN" ]]; then
    echo "missing $BIN" >&2
    exit 3
fi

run() { # $1 = platform, $2 = display (empty for none)
    if [[ -n "$2" ]]; then
        DISPLAY="$2" QT_QPA_PLATFORM="$1" "$BIN"
    else
        QT_QPA_PLATFORM="$1" "$BIN"
    fi
}

set +e
echo "--- running with QT_QPA_PLATFORM=${AE_QT_PLATFORM:-offscreen} ---"
run "${AE_QT_PLATFORM:-offscreen}" ""
code=$?
if [[ $code -eq 2 ]]; then
    # no offscreen GL: retry on the real display, like tools/render_verify does
    if [[ -n "${DISPLAY:-}" ]]; then
        echo "--- retrying with QT_QPA_PLATFORM=xcb on DISPLAY=$DISPLAY ---"
        run xcb "$DISPLAY"
        code=$?
    fi
fi
exit $code
