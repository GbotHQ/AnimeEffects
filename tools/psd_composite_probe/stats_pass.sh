#!/usr/bin/env bash
# Per-layer alpha statistics for every PSD in a list, as one TSV: the numbers the artist asked for
# (max alpha, mean over alpha>2, count(alpha>10), count(2<alpha<=10), and their ratio A) plus the
# level each candidate rule derives from them.
#
#   stats_pass.sh <file-list> <outDir>
#
# GL-free (threshold_audit decodes the layers and computes a histogram; it never renders), so this
# can run next to a render pass - but it still decodes whole layers, so it has its own memory guard
# and never starts a file while the machine is short of memory.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
LIST="$1"; OUT="$2"
mkdir -p "$OUT"
ALL="$OUT/all.tsv"
: > "$ALL"
mem_guard() {
    local need="${AE_MIN_FREE_GB:-6}"
    for _ in $(seq 1 240); do
        local free_gb
        free_gb=$(awk '/^MemAvailable/ {printf "%d", $2/1048576}' /proc/meminfo)
        if (( free_gb >= need )); then return 0; fi
        echo "    waiting for memory: ${free_gb}GB available, need ${need}GB"
        sleep 15
    done
    return 1
}
while read -r psd; do
    [[ -z "$psd" ]] && continue
    name="$(basename "$psd" .psd)"
    [[ -s "$OUT/$name.tsv" ]] && { echo "  $name kept"; cat "$OUT/$name.tsv" >> "$ALL"; continue; }
    mem_guard || break
    echo "  $name $(date +%H:%M:%S)  (mem avail $(awk '/^MemAvailable/ {printf "%d", $2/1048576}' /proc/meminfo)GB)"
    AE_BUILD_DIR=build timeout 1800 tools/psd_composite_probe/build_and_run.sh --thresholds --no-build \
        "$psd" "$OUT/sheets/$name" 2>/dev/null | grep -E "^(STATS|CELLS) " > "$OUT/$name.tsv"
    cat "$OUT/$name.tsv" >> "$ALL"
    echo "    $(grep -c '^STATS' "$OUT/$name.tsv") layers"
done < "$LIST"
echo "DONE $(date +%H:%M:%S)  $(grep -c '^STATS' "$ALL") layers in $ALL"
