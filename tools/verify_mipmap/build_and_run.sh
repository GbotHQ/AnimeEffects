#!/usr/bin/env bash
# Builds and runs the mipmap verification probe against the project's Debug libraries.
# Usage: tools/verify_mipmap/build_and_run.sh [--no-build] [probe args...]
#
# Mirrors tools/verify_blur/build_and_run.sh: build from the repo root, try offscreen GL
# first and fall back to X11 when offscreen GL is unavailable (exit code 2 signals a GL
# bootstrap failure, not a test failure).
#
# Unlike verify_blur this probe loads no shaders (it reads mip levels back with
# glGetTexImage and drives the production gl::Texture / core::ImageKey directly), so it
# needs no assets.qrc and no rcc.
#
# Set AE_MIPMAP_ASAN=1 to build against build-asan/ instead (see the project README for
# configuring that tree with -DAE_ENABLE_ASAN=ON). The default uses build/Debug so the
# probe can be iterated without a second full build.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="build"
CONFIG="Debug"
ASAN_FLAGS=""
if [[ "${AE_MIPMAP_ASAN:-0}" == "1" ]]; then
    BUILD_DIR="build-asan"
    ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
    export ASAN_OPTIONS=abort_on_error=1:detect_leaks=0
fi

QT_MODULES="Qt6Core Qt6Gui Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets Qt6Xml Qt6Core5Compat Qt6Multimedia"

if [[ "${1:-}" != "--no-build" ]]; then
    cmake --build "$BUILD_DIR" --config "$CONFIG"

    clang++ -std=c++17 -fPIC -O1 -g $ASAN_FLAGS \
        -DUSE_GL_CORE_PROFILE \
        -Isrc -Isrc/deps -Isrc/deps/pugixml/src \
        $(pkg-config --cflags $QT_MODULES) \
        tools/verify_mipmap/mip_probe.cpp \
        -Wl,--start-group \
        "$BUILD_DIR"/src/core/$CONFIG/libcore.a \
        "$BUILD_DIR"/src/ctrl/$CONFIG/libctrl.a \
        "$BUILD_DIR"/src/cmnd/$CONFIG/libcmnd.a \
        "$BUILD_DIR"/src/img/$CONFIG/libimg.a \
        "$BUILD_DIR"/src/gl/$CONFIG/libgl.a \
        "$BUILD_DIR"/src/thr/$CONFIG/libthr.a \
        "$BUILD_DIR"/src/util/$CONFIG/libutil.a \
        "$BUILD_DIR"/src/deps/pugixml/$CONFIG/libpugixml.a \
        -Wl,--end-group \
        $(pkg-config --libs $QT_MODULES) -lGL \
        -o "$BUILD_DIR"/verify_mipmap
else
    shift # --no-build consumed; the rest goes to the probe
fi

run() { # $1 = Qt platform, rest = probe args
    local platform="$1"; shift
    echo "--- running with QT_QPA_PLATFORM=$platform ---"
    QT_QPA_PLATFORM="$platform" "./$BUILD_DIR/verify_mipmap" "$@"
}

set +e
run offscreen "$@"
code=$?
if [[ $code -eq 2 ]]; then
    echo "offscreen GL unavailable, retrying on X11 (DISPLAY=${DISPLAY:-:0})"
    QT_QPA_PLATFORM=xcb DISPLAY="${DISPLAY:-:0}" "./$BUILD_DIR/verify_mipmap" "$@"
    code=$?
fi
exit $code
