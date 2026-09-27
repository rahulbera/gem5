# misc: Log perf-opt trial 19 (O3 width and thread bounds)

- **Date:** 2026-09-27 07:22   ·   **Branch:** feat/opt

## Goal
Record a rejected part-2 trial whose result is a correctness finding.

## Summary of changes
Trial 19 built with MaxWidth = 8 and MaxThreads = 1 (the config uses at
most 8-wide stages and one thread). It was faster on 5/5 but changed about
2,000 stats on every checkpoint, the same scale as the x86-64-v3 build of
Trial 5b. No code path uses the bounds as values, so the likeliest cause is
a read of uninitialized memory whose contents depend on layout (known
instance: LoopPredictor::BranchInfo::loopPredUsed). The source change is
not kept; its patch is.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — Trial 19 entry.
- `docs/perf-opt/patches/t19-maxwidth-limits.patch` — the rejected change.
