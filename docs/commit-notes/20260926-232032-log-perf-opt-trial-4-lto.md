# misc,util: Log perf-opt trial 4 (LTO)

- **Date:** 2026-09-26 23:20   ·   **Branch:** feat/opt

## Goal
Record the fourth ladder trial: GCC LTO, compared across the bfd, gold and
mold linkers.

## Summary of changes
LTO cuts .text from 42.2 MB to 29.8 MB and is faster on 5/5 checkpoints
with stats bit-identical, for every linker; the linker difference (~1%)
is within noise. Accepted with bfd (best quiet-start mean, +3.93%;
cumulative +35-39% over the .opt baseline). run_suite.sh now waits for the
1-minute load average to drop below 1.0 before starting a trial, because
it lags after a previous trial; gold/mold were re-run under that guard.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — trial 4 subsection (both passes, link times).
- `util/perf-opt/run_suite.sh` — wait for a quiet machine (load1 < 1.0, max 5 min) and record the wait.
