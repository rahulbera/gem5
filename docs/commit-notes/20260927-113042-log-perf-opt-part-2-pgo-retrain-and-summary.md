# misc: Log perf-opt part 2 PGO retrain and summary

- **Date:** 2026-09-27 11:30   ·   **Branch:** feat/opt

## Goal
Close part 2 of the perf-opt campaign: record the PGO retrain on the final
code and the end-to-end result.

## Summary of changes
- PGO retrained on the part-2 sources (SPEC-trained, local g++ 12.4): +11%
  to +19% on top of Trial 21, stats bit-identical.
- Part-2 summary: 16 trials (10 accepted, 6 rejected) plus the loopPredUsed
  fix. Back-to-back end-to-end comparison: development build +17.3%
  geomean over the part-2 baseline, PGO build +35.9%, all bit-identical.
  Chained per-trial gains (+22.7%) overstate because of selection bias.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — PGO retrain entry and part-2 summary.
