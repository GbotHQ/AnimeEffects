#!/usr/bin/env bash
# Builds the directional-blur verification harness against the libraries in a build tree
# and runs it from the repo root.
#
#   AE_BUILD_DIR   build tree to link against (default: build-asan, the ASan build the
#                  suite was written for). Point it at an ordinary build (e.g. build) when
#                  no ASan tree exists; the harness then runs without sanitizer coverage.
#
# The shaders are loaded from the embedded Qt resources (:/shaders/*), so assets.qrc is
# compiled into the harness binary here; without it the first LayerNode aborts with an
# XC_FATAL_ERROR shader FileIO error.
#
# Usage: tools/render_verify/build_and_run.sh [--no-build] [harness args...]
#   harness args are passed to the binary, e.g.:
#     tools/render_verify/build_and_run.sh --no-build --golden-gen   # regenerate golden inputs
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${AE_BUILD_DIR:-build-asan}"
ASAN_FLAGS="${AE_SANITIZE_FLAGS--fsanitize=address -fno-omit-frame-pointer}"
QT_MODULES="Qt6Core Qt6Gui Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets Qt6Xml Qt6Core5Compat Qt6Multimedia Qt6MultimediaWidgets"
# ctrl::Exporter (exercised by S9) goes through the media library, which owns the FFmpeg
# linkage; both must be on the link line or the harness no longer links.
FFMPEG_MODULES="libavformat libavutil libswscale libswresample libavcodec"
export ASAN_OPTIONS=abort_on_error=1:detect_leaks=0

if [[ "${1:-}" != "--no-build" ]]; then
    cmake --build "$BUILD_DIR" --config Debug

    # assets.qrc -> embedded shaders/icons (the GUI target normally provides these; the
    # harness links the libraries only, so it must link the resource itself)
    RCC="${RCC:-}"
    if [[ -z "$RCC" ]]; then
        if command -v qtpaths6 >/dev/null; then
            RCC="$(qtpaths6 --query QT_INSTALL_LIBEXECS)/rcc"
        else
            RCC=/usr/lib64/qt6/libexec/rcc
        fi
    fi
    "$RCC" --name assets --output "$BUILD_DIR/qrc_assets.cpp" assets/assets.qrc

    clang++ -std=c++17 -fPIC -O1 $ASAN_FLAGS \
        -DUSE_GL_CORE_PROFILE \
        -Isrc -Isrc/deps/pugixml/src -Itests/support \
        $(pkg-config --cflags $QT_MODULES) \
        tools/render_verify/render_verify.cpp tools/render_verify/scene.cpp tools/render_verify/golden.cpp \
        "$BUILD_DIR/qrc_assets.cpp" \
        -Wl,--start-group \
        "$BUILD_DIR"/src/core/Debug/libcore.a \
        "$BUILD_DIR"/src/ctrl/Debug/libctrl.a \
        "$BUILD_DIR"/src/media/Debug/libmedia.a \
        "$BUILD_DIR"/src/cmnd/Debug/libcmnd.a \
        "$BUILD_DIR"/src/img/Debug/libimg.a \
        "$BUILD_DIR"/src/gl/Debug/libgl.a \
        "$BUILD_DIR"/src/thr/Debug/libthr.a \
        "$BUILD_DIR"/src/util/Debug/libutil.a \
        "$BUILD_DIR"/src/deps/pugixml/Debug/libpugixml.a \
        -Wl,--end-group \
        $(pkg-config --libs $QT_MODULES) $(pkg-config --libs $FFMPEG_MODULES) -lGL \
        -o "$BUILD_DIR/render_verify"
else
    shift # --no-build consumed; the rest goes to the binary
fi

run() { # $1 = Qt platform, rest = harness args
    local platform="$1"; shift
    echo "--- running with QT_QPA_PLATFORM=$platform ---"
    QT_QPA_PLATFORM="$platform" "./$BUILD_DIR/render_verify" "$@"
}

set +e
run offscreen "$@"
code=$?
if [[ $code -eq 2 ]]; then
    # exit code 2 = GL bootstrap failure; retry on the real display
    echo "offscreen GL unavailable, retrying on X11 (DISPLAY=${DISPLAY:-:0})"
    QT_QPA_PLATFORM=xcb DISPLAY="${DISPLAY:-:0}" "./$BUILD_DIR/render_verify" "$@"
    code=$?
fi
exit $code
