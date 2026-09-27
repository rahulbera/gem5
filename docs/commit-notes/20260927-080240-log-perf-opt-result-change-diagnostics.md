# misc: Log perf-opt result-change diagnostics

- **Date:** 2026-09-27 08:02   ·   **Branch:** feat/opt

## Goal
Record three diagnostics run to explain the result changes of perf-opt
Trials 5b and 19. No source change.

## Summary of changes
- Pattern-initialized stack (-ftrivial-auto-var-init=pattern): identical.
- glibc malloc instead of tcmalloc: identical.
- glibc with MALLOC_PERTURB_=165: 52-55 branch-predictor bookkeeping stats
  differ (loop_predictor.used 0 -> 228,618 on llvm), timing unchanged.
  This confirms the uninitialized LoopPredictor::BranchInfo::loopPredUsed
  read, but shows it is not what changes timing in Trials 5b and 19.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — diagnostics section.
