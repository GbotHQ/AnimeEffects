#!/usr/bin/env bash
# Builds the PSD composite probe against the static libraries in a build tree and runs it
# from the repo root.
#
#   AE_BUILD_DIR   build tree to link against (default: build). The tools only link the
#                  tree's static libraries; they do not rebuild them, so re-run
#                  `cmake --build "$AE_BUILD_DIR" --config Debug` after touching src/, or
#                  pass --libs to have this script do it first.
#   AE_PROBE_CXX   compiler (default: clang++)
#   RCC            rcc binary (default: `qtpaths6 --query QT_INSTALL_LIBEXECS`/rcc, then
#                  /usr/lib64/qt6/libexec/rcc)
#   PROBE_PSD      PSD analysed when no positional arguments are given (default: the
#                  114 MB "ALL FATHER F3 color fixes.psd" in ~/Downloads)
#   PROBE_OUT      output directory for the probe's PNG/dump output (default:
#                  /tmp/psd_composite_probe -- deliberately outside the repo)
#   PROBE_FRAME    frame rendered by the probe (default: 0)
#   PROBE_BLEND, PROBE_GREEDY, PROBE_LAYER_DUMP, PROBE_GROUP, PROBE_LAYER
#                  read by the probe itself; see README.md
#
# The shaders are loaded from the embedded Qt resources (:/shaders/*), so assets/assets.qrc
# is compiled into the probe binary here; without it the first LayerNode aborts with an
# XC_FATAL_ERROR shader FileIO error. The generated qrc source and the probe binary are
# written into "$AE_BUILD_DIR" (git-ignored), never into the tool directory.
#
# Usage: tools/psd_composite_probe/build_and_run.sh [--build-only] [--no-build] [--libs]
#            [--cost | --mesh | --audit] [<psd> <composite.png> <outDir> [frame]]
#   --build-only   compile and exit without running (alias: --no-run)
#   --no-build     skip compilation, run the existing binary
#   --libs         `cmake --build "$AE_BUILD_DIR" --config Debug` before compiling
#   with no positional arguments the probe is run on PROBE_PSD / PROBE_OUT. The composite
#   PNG in argv[2] (default "$PROBE_OUT/composite.png") is created with
#   extract_composite.py when it is missing.
#
# Three modes build and run a different, GL-free tool on one PSD instead of the probe. Each has
# its own binary name, so a later --no-build run executes the tool the mode names rather than
# whichever was built last:
#
#   --cost         reload_cost.cpp: where a source reload's time goes (parse / structure-only
#                  description / full decode / digest of the compressed bytes).
#                    build_and_run.sh --cost [<psd>]
#   --mesh         mesh_policy.cpp: the three candidate policies for the mesh's content test,
#                  with the geometry each one costs.
#                    build_and_run.sh --mesh [<psd>] [--seed T]
#   --audit        soft_content_audit.cpp: what a candidate content rule would drop, by the
#                  dropped regions' own distribution and boundary contrast, plus amplified crops
#                  of the largest ones under "$PROBE_OUT/audit".
#                    build_and_run.sh --audit [<psd>] [--seed T] [--min-size N] [--top N]
#   --thresholds   threshold_audit.cpp: what a cutoff on a region's MAX or MEAN alpha removes,
#                  as before/after/removed/difference-x16 strips plus the numbers. Everything
#                  after the mode name goes to the tool unchanged. The mode is NOT named --cut,
#                  because --cut C is the tool's own option and one name cannot mean both.
#                    build_and_run.sh --thresholds <psd> <outDir> [--policy max|mean] [--cut C]
#                                     [--top N] [--scale S]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${AE_BUILD_DIR:-build}"
CXX="${AE_PROBE_CXX:-clang++}"
# The corpus file the probe was written against is NOT a path in this repository: pass it, or set
# PROBE_PSD (the same variable the python tools read as AE_PROBE_PSD) to a file you have.
PSD="${PROBE_PSD:-${AE_PROBE_PSD:-}}"
if [[ -z "$PSD" ]]; then
    echo "no PSD given: pass one as the first argument or set PROBE_PSD" >&2
    exit 64
fi
OUT_DIR="${PROBE_OUT:-/tmp/psd_composite_probe}"
PROBE_BIN="$BUILD_DIR/psd_composite_probe"
COST_BIN="$BUILD_DIR/reload_cost"
MESH_BIN="$BUILD_DIR/mesh_policy"
AUDIT_BIN="$BUILD_DIR/soft_content_audit"
CUT_BIN="$BUILD_DIR/threshold_audit"
QRC_CPP="$BUILD_DIR/qrc_assets_psd_probe.cpp"
QRC_OBJ="$BUILD_DIR/qrc_assets_psd_probe.o"

QT_MODULES="Qt6Core Qt6Gui Qt6Widgets Qt6OpenGL Qt6OpenGLWidgets Qt6Xml Qt6Core5Compat Qt6Multimedia Qt6MultimediaWidgets"
# ctrl::ImageFileLoader reaches img/ and media/ (FFmpeg codecs), so the media library and
# both Qt Multimedia and the FFmpeg modules must be on the link line or the probe no
# longer links.
FFMPEG_MODULES="libavformat libavutil libswscale libswresample libavcodec"

BUILD_ONLY=0
DO_BUILD=1
DO_LIBS=0
DO_COST=0
DO_MESH=0
DO_AUDIT=0
DO_CUT=0
POS=()
for arg in "$@"; do
    case "$arg" in
        --build-only|--no-run) BUILD_ONLY=1 ;;
        --no-build) DO_BUILD=0 ;;
        --libs) DO_LIBS=1 ;;
        --cost) DO_COST=1 ;;
        --mesh) DO_MESH=1 ;;
        --audit) DO_AUDIT=1 ;;
        --thresholds) DO_CUT=1 ;;
        -h|--help) sed -n '/^# Usage:/,/^#   with no positional/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) POS+=("$arg") ;;
    esac
done

# The mode chooses its binary HERE, not inside the build block: with --no-build no build happens,
# and the run below must still execute the tool the mode names.
if [[ $DO_COST -eq 1 ]]; then
    PROBE_BIN="$COST_BIN"
elif [[ $DO_MESH -eq 1 ]]; then
    PROBE_BIN="$MESH_BIN"
elif [[ $DO_AUDIT -eq 1 ]]; then
    PROBE_BIN="$AUDIT_BIN"
elif [[ $DO_CUT -eq 1 ]]; then
    PROBE_BIN="$CUT_BIN"
fi

# The tools take one PSD, optionally preceded/followed by their own options. With no PSD given,
# PROBE_PSD is used; a leading option (--seed 4) must not be mistaken for the file.
positional_file_first() {
    local -n out=$1
    shift
    out=("$@")
    if [[ ${#out[@]} -eq 0 ]]; then
        out=("$PSD")
    elif [[ "${out[0]}" == -* ]]; then
        out=("$PSD" "${out[@]}")
    fi
}

if [[ $DO_BUILD -eq 1 ]]; then
    if [[ $DO_LIBS -eq 1 ]]; then
        cmake --build "$BUILD_DIR" --config Debug
    fi

    for lib in core ctrl media cmnd img gl thr util; do
        if [[ ! -f "$BUILD_DIR/src/$lib/Debug/lib$lib.a" ]]; then
            echo "missing $BUILD_DIR/src/$lib/Debug/lib$lib.a -- build the tree first:" >&2
            echo "  cmake --build $BUILD_DIR --config Debug   (or pass --libs)" >&2
            exit 1
        fi
    done
    if [[ ! -f "$BUILD_DIR/src/deps/pugixml/Debug/libpugixml.a" ]]; then
        echo "missing $BUILD_DIR/src/deps/pugixml/Debug/libpugixml.a -- build the tree first" >&2
        exit 1
    fi

    # assets.qrc -> embedded shaders/icons (the GUI target normally provides these; the
    # probe links the libraries only, so it must link the resource itself)
    RCC="${RCC:-}"
    if [[ -z "$RCC" ]]; then
        if command -v qtpaths6 >/dev/null; then
            RCC="$(qtpaths6 --query QT_INSTALL_LIBEXECS)/rcc"
        else
            RCC=/usr/lib64/qt6/libexec/rcc
        fi
    fi
    # NOTE: the check must cover every file the .qrc pulls in, not just the .qrc itself --
    # assets.qrc keeps its own (old) mtime when only a shader changes, and a stale embedded
    # shader silently invalidates every render the probe reports.
    if [[ ! -f "$QRC_CPP" || -n "$(find assets -newer "$QRC_CPP" -print -quit)" ]]; then
        "$RCC" --name assets --output "$QRC_CPP" assets/assets.qrc
    fi

    CXXFLAGS=(
        -std=c++17 -fPIC -g -O1
        -DUSE_GL_CORE_PROFILE
        -DAE_MAJOR_VERSION=1 -DAE_MINOR_VERSION=7 -DAE_MICRO_VERSION=1
        -Isrc -Isrc/deps/pugixml/src -Itools/render_verify
    )
    # the generated resource is ~10 MB of C++ and changes rarely: keep its object cached
    if [[ ! -f "$QRC_OBJ" || "$QRC_CPP" -nt "$QRC_OBJ" ]]; then
        "$CXX" "${CXXFLAGS[@]}" $(pkg-config --cflags $QT_MODULES) -c "$QRC_CPP" -o "$QRC_OBJ"
    fi

    # none of the measurement tools needs shaders or a scene, so they do not link the embedded
    # assets the probe needs
    if [[ $DO_COST -eq 1 ]]; then
        SOURCES=(tools/psd_composite_probe/reload_cost.cpp)
    elif [[ $DO_MESH -eq 1 ]]; then
        SOURCES=(tools/psd_composite_probe/mesh_policy.cpp)
    elif [[ $DO_AUDIT -eq 1 ]]; then
        SOURCES=(tools/psd_composite_probe/soft_content_audit.cpp)
    elif [[ $DO_CUT -eq 1 ]]; then
        SOURCES=(tools/psd_composite_probe/threshold_audit.cpp)
    else
        SOURCES=(tools/psd_composite_probe/psd_composite_probe.cpp tools/render_verify/scene.cpp "$QRC_OBJ")
    fi

    "$CXX" "${CXXFLAGS[@]}" $(pkg-config --cflags $QT_MODULES) \
        "${SOURCES[@]}" \
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
        -o "$PROBE_BIN"
    echo "built $PROBE_BIN"
fi

# The measurement modes need no GL, no composite and no frame: run and exit.
if [[ $DO_COST -eq 1 ]]; then
    exec "./$PROBE_BIN" "${POS[0]:-$PSD}"
fi
if [[ $DO_CUT -eq 1 ]]; then
    # threshold_audit takes <psd> <outDir> [options]; with no outDir it writes under PROBE_OUT
    cutPos=()
    positional_file_first cutPos "${POS[@]}"
    mkdir -p "$OUT_DIR/thresholds"
    if [[ ${#cutPos[@]} -gt 1 && "${cutPos[1]}" != -* ]]; then
        exec "./$PROBE_BIN" "${cutPos[@]}"
    fi
    exec "./$PROBE_BIN" "${cutPos[0]}" "$OUT_DIR/thresholds" "${cutPos[@]:1}"
fi
if [[ $DO_MESH -eq 1 ]]; then
    meshPos=()
    positional_file_first meshPos "${POS[@]}"
    exec "./$PROBE_BIN" "${meshPos[@]}"
fi
if [[ $DO_AUDIT -eq 1 ]]; then
    # the audit's second positional is an output directory; give it PROBE_OUT/audit so the crops
    # are written where every other dump goes, whether or not the caller passed one
    auditPos=()
    positional_file_first auditPos "${POS[@]}"
    mkdir -p "$OUT_DIR/audit"
    if [[ ${#auditPos[@]} -gt 1 && "${auditPos[1]}" != -* ]]; then
        exec "./$PROBE_BIN" "${auditPos[@]}"
    fi
    exec "./$PROBE_BIN" "${auditPos[0]}" "$OUT_DIR/audit" "${auditPos[@]:1}"
fi

if [[ $BUILD_ONLY -eq 1 ]]; then
    exit 0
fi

# argv: <psd> <composite.png> <outDir> [frame]
if [[ ${#POS[@]} -eq 0 ]]; then
    mkdir -p "$OUT_DIR"
    POS=("$PSD" "$OUT_DIR/composite.png" "$OUT_DIR" "${PROBE_FRAME:-0}")
elif [[ ${#POS[@]} -lt 3 ]]; then
    echo "usage: $0 [--build-only] [--no-build] [--libs] <psd> <composite.png> <outDir> [frame]" >&2
    exit 64
fi

# the reference composite comes from the PSD's own section 5; produce it on demand so the
# probe always diffs against a file decoded from this exact PSD (see README.md)
if [[ ! -f "${POS[1]}" ]]; then
    mkdir -p "$(dirname -- "${POS[1]}")"
    python3 tools/psd_composite_probe/extract_composite.py "${POS[0]}" "${POS[1]}"
fi

run() { # $1 = Qt platform; remaining positional args are already in POS
    local platform="$1"
    echo "--- running with QT_QPA_PLATFORM=$platform ---"
    QT_QPA_PLATFORM="$platform" "./$PROBE_BIN" "${POS[@]}"
}

set +e
run offscreen
code=$?
if [[ $code -eq 2 ]]; then
    # exit code 2 = GL bootstrap failure; retry on the real display
    echo "offscreen GL unavailable, retrying on X11 (DISPLAY=${DISPLAY:-:0})"
    QT_QPA_PLATFORM=xcb DISPLAY="${DISPLAY:-:0}" "./$PROBE_BIN" "${POS[@]}"
    code=$?
fi
exit $code
