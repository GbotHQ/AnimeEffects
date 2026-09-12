#!/usr/bin/env bash
# Renders every PSD in a list once per content cutoff, so the only difference between two renders of
# the same file is the cutoff.
#
#   cutoff_batch.sh <file-list> <outDir> [<variant> ...]
#     variants: constant | none | fixed:<T> | mean | ratio:<anchor>
#                (default: constant none mean ratio:0.55 - the anchor the artist picked)
#
# The cutoff is a MESH-CREATION PARAMETER (GridMesh::createFromImage), not a mask over the pixels:
# a mask cannot express a threshold below production's 10 (see the note in psd_composite_probe.cpp
# and S23 in verify_blur).
#
#   constant (dir const10)  T=10, what ships today - the reference every other variant is read against
#   none     (dir nocut)    T=0, alpha != 0: nothing is cut, the "is anything still lost" reference
#
# PROBE_BIN pins the binary to run (default: tools/psd_composite_probe/build_and_run.sh --no-build,
# i.e. "$AE_BUILD_DIR"/psd_composite_probe). Pin it when comparing two revisions of the mesh code in
# one output tree, so a rebuild cannot silently change which engine a run used.
# SKIP_EXISTING=1 reuses a render already on disk.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
LIST="$1"; OUT="$2"; shift 2
VARIANTS=("$@")
[[ ${#VARIANTS[@]} -eq 0 ]] && VARIANTS=(constant none mean ratio:0.55)
PROBE_BIN="${PROBE_BIN:-}"

# the directory each variant writes into: the names the report site uses
dir_for() {
    case "$1" in
        constant) echo const10 ;;
        none) echo nocut ;;
        fixed) echo "fixed$2" ;;
        ratio) echo "ratio$2" ;;
        *) echo "$1" ;;
    esac
}
# how to run the probe: the pinned binary, or the build script (which knows the include/link flags).
# The pinned path has to reproduce what build_and_run.sh does around the binary, or the run silently
# loses everything except the render: the PSD's own stored composite is extracted on demand (the
# probe diffs against it - without it there are no whole-canvas numbers), and the Qt platform is
# pinned to offscreen.
run_probe() {  # $1 = psd, $2 = composite, $3 = outDir
    if [[ -n "$PROBE_BIN" ]]; then
        if [[ ! -f "$2" ]]; then
            mkdir -p "$(dirname -- "$2")"
            python3 tools/psd_composite_probe/extract_composite.py "$1" "$2" >/dev/null
        fi
        QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" timeout 3600 "$PROBE_BIN" "$1" "$2" "$3" 0
    else
        AE_BUILD_DIR=build timeout 3600 tools/psd_composite_probe/build_and_run.sh --no-build \
            "$1" "$2" "$3" 0
    fi
}

# MEMORY GUARD. The probe holds the whole file resident (the largest corpus PSDs reached ~4.6 GB),
# and an unbounded number of concurrent probes OOMed this machine and destroyed a batch's output.
# Nothing starts while the machine is short of memory, and the caller is expected to run at most a
# couple of this script at once.
mem_guard() {
    local need="${AE_MIN_FREE_GB:-8}"
    for _ in $(seq 1 240); do
        local free_gb
        free_gb=$(awk '/^MemAvailable/ {printf "%d", $2/1048576}' /proc/meminfo)
        if (( free_gb >= need )); then
            return 0
        fi
        echo "    waiting for memory: ${free_gb}GB available, need ${need}GB"
        sleep 15
    done
    echo "    memory still short of ${need}GB after 60 min; aborting this list" >&2
    return 1
}
mkdir -p "$OUT"
while read -r psd; do
    [[ -z "$psd" ]] && continue
    name="$(basename "$psd" .psd)"
    mem_guard || break
    echo "##### $name  $(date +%H:%M:%S)  (mem avail $(awk '/^MemAvailable/ {printf "%d", $2/1048576}' /proc/meminfo)GB)  engine $(basename "${PROBE_BIN:-$AE_BUILD_DIR/psd_composite_probe}")"
    for v in "${VARIANTS[@]}"; do
        spec="${v%%:*}"; val="${v##*:}"
        [[ "$spec" == "$val" ]] && val=0.55
        vdir="$(dir_for "$spec" "$val")"
        if [[ -n "${SKIP_EXISTING:-}" && -s "$OUT/$name/$vdir/render_straight.png" ]]; then
            echo "    $vdir   kept (SKIP_EXISTING)"
            continue
        fi
        PROBE_CUTOFF="$spec,$val" run_probe "$psd" "$OUT/composite_$name.png" "$OUT/$name/$vdir" 2>&1 \
            | grep -E "load\(\)=|whole canvas|cells<=T|FATAL" | tail -4 | sed "s/^/    $vdir   /"
    done
done < "$LIST"
echo "DONE $(date +%H:%M:%S)"
