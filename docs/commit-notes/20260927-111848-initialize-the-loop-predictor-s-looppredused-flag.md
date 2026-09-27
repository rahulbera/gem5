# cpu: Initialize the loop predictor's loopPredUsed flag

- **Date:** 2026-09-27 11:18   ·   **Branch:** feat/opt

## Goal
Fix a read of uninitialized memory in the loop predictor, found by the
perf-opt diagnostics and approved by the user.

## Summary of changes
LoopPredictor::BranchInfo's constructor left loopPredUsed uninitialized; it
is only ever set to true, yet read on every prediction (TAGE-SC-L provider
relabelling and the loop_predictor.used/correct/wrong stats). It now starts
as false. With tcmalloc the garbage read as 0, so results are unchanged
(bit-identical on all five test checkpoints); with glibc and
MALLOC_PERTURB_=165 the 52-55 branch-predictor stats that used to differ
are now stable.

## Files changed
- `src/cpu/pred/loop_predictor.hh` — initialize loopPredUsed to false.
- `docs/perf-opt/performance-opt-log.md` — fix entry and verification.
