# misc: Log perf-opt trial 3 (optimized ext)

- **Date:** 2026-09-26 23:00   ·   **Branch:** feat/opt

## Goal
Record the third ladder trial: compiling ext/ (DRAMPower etc.) at -O3.

## Summary of changes
ext/ is built at -O0 in every variant; CCFLAGS_EXTRA=-O3 fixes that.
Stats bit-identical; +3.3-3.4% on the two memory-heavy checkpoints, within
noise on sqlite, -0.9%/-1.8% on the compute-bound stockfish pair. Accepted
narrowly (3/5 faster, one within noise), with that caveat recorded.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — trial 3 subsection.
