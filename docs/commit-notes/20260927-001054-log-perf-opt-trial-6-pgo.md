# misc: Log perf-opt trial 6 (PGO)

- **Date:** 2026-09-27 00:10   ·   **Branch:** feat/opt

## Goal
Record the profile-guided optimization trial.

## Summary of changes
GCC PGO trained on three checkpoints disjoint from the test suite is
8.1-11.3% faster on all five test checkpoints with stats bit-identical
(cumulative +49.5-53.2% over the .opt baseline). Accepted by the gate.
Records the pipeline cost (~16 min per retrain vs ~5.4 min for a plain LTO
build), why a profile goes stale only gradually, and the open workflow
decision the user raised.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — trial 6 subsection.
