#!/usr/bin/env bash
# Profile only the measured (detailed) region of one fs_run.py restore.
#
# gem5 runs pinned to core 1 with the suite's 10M warmup + 30M detailed
# window. When fs_run.py prints its "(measured)" marker, the profiler command
# attaches to the gem5 process; perf stat/record -p stop by themselves when
# gem5 exits. Startup and checkpoint restore are therefore excluded.
#
# Usage: profile_region.sh <gem5-binary> <checkpoint-id> <outdir> <profiler...>
#   Every literal "PID" in <profiler...> is replaced by gem5's process id, e.g.
#     profile_region.sh build/X/gem5.fast 723.llvm_r.1.0 /tmp/p \
#         perf stat -M PipelineL1 -p PID
set -euo pipefail
BIN=$(readlink -f "$1")
CID="$2"
OUT="$3"
shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
GEM5=$(cd "$HERE/../.." && pwd)
RES=/home/rbera/work/tracezoo/gem5/restore_resources

read -r CPT BENCH INV < <(python3 -c '
import json, sys
for c in json.load(open(sys.argv[1]))["checkpoints"]:
    if c["id"] == sys.argv[2]:
        print(c["local"], c["benchmark"], c["inv"])
' "$HERE/suite.json" "$CID")
[ -n "${CPT:-}" ] || { echo "unknown checkpoint id: $CID" >&2; exit 2; }
if [ -e "$OUT" ]; then
    echo "output dir exists, refusing to overwrite: $OUT" >&2
    exit 2
fi
mkdir -p "$OUT"

PYTHONUNBUFFERED=1 env -u LD_LIBRARY_PATH GEM5_RESOURCE_DIR="$RES" \
    taskset -c 1 "$BIN" --outdir="$OUT/m5out" \
    "$GEM5/configs/garfield/arm/fs_run.py" \
    --restore-dir "$CPT" --benchmark "$BENCH" --inv "$INV" \
    --disk-img "$RES/spec_shared_root.img" --mem-size 16GiB \
    --warmup-insts 10000000 --detailed-insts 30000000 \
    > "$OUT/sim.log" 2>&1 &
PID=$!

until grep -q '(measured)' "$OUT/sim.log" 2>/dev/null; do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "gem5 exited before the measured region; see $OUT/sim.log" >&2
        exit 1
    fi
    sleep 0.2
done

cmd=()
for a in "$@"; do
    cmd+=("${a//PID/$PID}")
done
echo "attached at $(date -Is): ${cmd[*]}" > "$OUT/profiler.cmd"
"${cmd[@]}" > "$OUT/profiler.out" 2>&1 || true
wait "$PID"
echo "gem5 exit: $?; region KIPS: $(awk '$1 == "hostInstRate" {printf "%.1f", $2 / 1000}' "$OUT/m5out/stats.txt")"
