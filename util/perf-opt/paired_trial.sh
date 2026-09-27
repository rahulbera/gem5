#!/usr/bin/env bash
# Run one paired perf-opt trial: the current accepted binary (reference),
# then the candidate, back to back on the test suite, each after
# run_suite.sh's wait for a quiet machine. The candidate is gated for
# bit-identical stats against the identity reference, and its KIPS are
# compared with the fresh reference run rather than an older one, so that
# drift in machine state between trials does not decide the verdict.
#
# Usage: paired_trial.sh <reference-binary> <candidate-binary> <trial-name>
# Env:   PAIRED_BASE  space-separated baseline trial names for the
#                     cumulative column (default: "p2-base-a p2-base-b")
#        PAIRED_IDENT identity reference trial (default: trial1-fast)
# Output: $PERF_OPT_RUNS/<trial>-ref/ and $PERF_OPT_RUNS/<trial>/, plus
#         $PERF_OPT_RUNS/<trial>/verdict.md with the gate and the table.
set -euo pipefail
REF="$1"
CAND="$2"
TRIAL="$3"
HERE=$(cd "$(dirname "$0")" && pwd)
RUNS="${PERF_OPT_RUNS:-/home/rbera/work/agentic-cpu/perf-opt-runs}"
read -r -a BASE <<< "${PAIRED_BASE:-p2-base-a p2-base-b}"
IDENT="${PAIRED_IDENT:-trial1-fast}"

"$HERE/run_suite.sh" "$REF" "$TRIAL-ref"
"$HERE/run_suite.sh" "$CAND" "$TRIAL"

base_args=()
for b in "${BASE[@]}"; do
    base_args+=(--base "$RUNS/$b")
done

{
    echo "## Gate: reference run vs $IDENT"
    python3 "$HERE/stats_gate.py" gate "$RUNS/$IDENT" "$RUNS/$TRIAL-ref" || true
    echo
    echo "## Gate: candidate vs $IDENT"
    python3 "$HERE/stats_gate.py" gate "$RUNS/$IDENT" "$RUNS/$TRIAL" || true
    echo
    echo "## KIPS: candidate vs paired reference"
    python3 "$HERE/kips_table.py" --trial "$RUNS/$TRIAL" \
        --prev "$RUNS/$TRIAL-ref" "${base_args[@]}" --ident "$RUNS/$IDENT"
} | tee "$RUNS/$TRIAL/verdict.md"
