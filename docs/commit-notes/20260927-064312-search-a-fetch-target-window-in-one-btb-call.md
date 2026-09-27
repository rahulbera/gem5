# cpu: Search a fetch-target window in one BTB call

- **Date:** 2026-09-27 06:43   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 17: replace the per-address BTB
calls of the fetch-target search with one call per fetch target.

## Summary of changes
- `BranchTargetBuffer::findFirstBranch()`: scan start, start + step, ...
  up to the first address at least width bytes past start; return whether
  a branch was found, its address and its static instruction. The default
  uses valid()/getInst(); SimpleBTB overrides it with probe().
- `BPredUnit::BTBFindFirstBranch()` forwards to it; `BAC::generateFetchTargets`
  uses it and drops the separate BTBGetInst lookup.
- Result: stats bit-identical on all five test checkpoints; 4/5 faster
  against a paired Trial 14 run (llvm only +0.2%, so partly noise).

## Files changed
- `src/cpu/pred/btb.hh` — findFirstBranch() with the default implementation.
- `src/cpu/pred/simple_btb.hh`, `src/cpu/pred/simple_btb.cc` — SimpleBTB override.
- `src/cpu/pred/bpred_unit.hh` — BTBFindFirstBranch().
- `src/cpu/o3/bac.cc` — the fetch-target search uses it.
- `docs/perf-opt/performance-opt-log.md` — Trial 17 entry.
