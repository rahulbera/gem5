#!/usr/bin/env bash
# Run one perf-opt trial: every checkpoint of one role in suite.json through
# configs/garfield/arm/fs_run.py (10M warmup + 30M detailed), concurrently,
# each pinned to its own physical core. Cores 1-5 are used; their SMT
# siblings (17-21) stay idle.
#
# Usage: run_suite.sh <gem5-binary> <trial-name> [test|train]
# Output: $PERF_OPT_RUNS/<trial>/<checkpoint-id>/{stats.txt,sim.log,exit_code,wall_seconds}
set -euo pipefail
BIN=$(readlink -f "$1")
TRIAL="$2"
ROLE="${3:-test}"
HERE=$(cd "$(dirname "$0")" && pwd)
GEM5=$(cd "$HERE/../.." && pwd)
RUNS="${PERF_OPT_RUNS:-/home/rbera/work/agentic-cpu/perf-opt-runs}"
RES=/home/rbera/work/tracezoo/gem5/restore_resources
CORES=(1 2 3 4 5)
WARMUP=10000000
DETAILED=30000000
TIMEOUT=1800

OUT="$RUNS/$TRIAL"
if [ -e "$OUT" ]; then
    echo "trial dir exists, refusing to overwrite: $OUT" >&2
    exit 2
fi
mkdir -p "$OUT"

# Start from a quiet machine: the 1-minute load average lags, so right after
# a previous trial it still reads ~4. Wait (up to 5 min) for it to fall
# below 1.0 so back-to-back trials start equally quiet.
waited=0
while awk '{ exit !($1 >= 1.0) }' /proc/loadavg && [ "$waited" -lt 300 ]; do
    sleep 10
    waited=$((waited + 10))
done

{
    echo "binary: $BIN"
    echo "sha256: $(sha256sum "$BIN" | cut -d' ' -f1)"
    echo "git: $(git -C "$GEM5" rev-parse HEAD)"
    echo "role: $ROLE"
    echo "start: $(date -Is)"
    echo "loadavg_at_start: $(cut -d' ' -f1-3 /proc/loadavg)"
    echo "waited_for_quiet_s: $waited"
    echo "tcmalloc: $(ldd "$BIN" | grep -o 'libtcmalloc[^ ]*' || echo none)"
} > "$OUT/meta.txt"

mapfile -t ROWS < <(python3 -c '
import json, sys
for c in json.load(open(sys.argv[1]))["checkpoints"]:
    if c["role"] == sys.argv[2]:
        print(c["id"], c["local"], c["benchmark"], c["inv"])
' "$HERE/suite.json" "$ROLE")
if [ "${#ROWS[@]}" -eq 0 ] || [ "${#ROWS[@]}" -gt "${#CORES[@]}" ]; then
    echo "need 1..${#CORES[@]} checkpoints for role '$ROLE', got ${#ROWS[@]}" >&2
    exit 2
fi

pids=()
for i in "${!ROWS[@]}"; do
    read -r id cpt bench inv <<< "${ROWS[$i]}"
    d="$OUT/$id"
    mkdir -p "$d"
    (
        start=$(date +%s.%N)
        set +e
        env -u LD_LIBRARY_PATH GEM5_RESOURCE_DIR="$RES" \
            timeout "$TIMEOUT" taskset -c "${CORES[$i]}" "$BIN" \
            --outdir="$d" "$GEM5/configs/garfield/arm/fs_run.py" \
            --restore-dir "$cpt" --benchmark "$bench" --inv "$inv" \
            --disk-img "$RES/spec_shared_root.img" --mem-size 16GiB \
            --warmup-insts "$WARMUP" --detailed-insts "$DETAILED" \
            > "$d/sim.log" 2>&1
        rc=$?
        end=$(date +%s.%N)
        echo "$rc" > "$d/exit_code"
        awk -v a="$start" -v b="$end" 'BEGIN { printf "%.1f\n", b - a }' \
            > "$d/wall_seconds"
    ) &
    pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || true; done
echo "end: $(date -Is)" >> "$OUT/meta.txt"

fail=0
for i in "${!ROWS[@]}"; do
    read -r id _ <<< "${ROWS[$i]}"
    rc=$(cat "$OUT/$id/exit_code")
    if [ "$rc" != 0 ] || [ ! -s "$OUT/$id/stats.txt" ]; then
        echo "FAIL $id (exit $rc; see $OUT/$id/sim.log)"
        fail=1
    else
        echo "ok   $id  wall $(cat "$OUT/$id/wall_seconds")s"
    fi
done
exit $fail
