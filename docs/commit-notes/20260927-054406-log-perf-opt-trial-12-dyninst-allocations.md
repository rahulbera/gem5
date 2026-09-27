# misc: Log perf-opt trial 12 (DynInst allocations)

- **Date:** 2026-09-27 05:44   ·   **Branch:** feat/opt

## Goal
Record a rejected part-2 trial and keep its patch.

## Summary of changes
Trial 12 backed DynInst::instResult with std::list (no allocation while
empty) and gave mispredicted() a reused scratch PC. Stats bit-identical,
but only 2/5 checkpoints faster; the gain is below the noise. The source
change is not kept.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — Trial 12 entry.
- `docs/perf-opt/patches/t12-dyninst-allocs.patch` — the rejected change.
