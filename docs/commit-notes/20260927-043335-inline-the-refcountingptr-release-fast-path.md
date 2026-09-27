# base,util: Inline the RefCountingPtr release fast path

- **Date:** 2026-09-27 04:33   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 7: stop paying a function call for
every RefCountingPtr release. The O3 CPU releases about 104 mostly-null
DynInstPtr per simulated cycle.

## Summary of changes
- `RefCountingPtr::del()` is inline again; only the delete moved into a new
  `GEM5_NO_INLINE static destroy(T *)`, which still hides the free from GCC
  and so keeps the false -Wuse-after-free of #2686 away.
- Result: stats bit-identical on all five test checkpoints; +2.8% to +6.5%
  KIPS against a paired run of the part-2 baseline binary. refcnt.test
  passes (8/8) and builds with mold without the warning.
- Adds `util/perf-opt/paired_trial.sh`, which reruns the reference binary
  right before each candidate and gates and tabulates the candidate.
- Log: part-2 preamble, part-2 baseline and Trial 7.

## Files changed
- `src/base/refcnt.hh` — inline del(); out-of-line destroy() holds only the delete.
- `util/perf-opt/paired_trial.sh` — paired reference/candidate trial driver.
- `docs/perf-opt/performance-opt-log.md` — part-2 preamble, baseline and Trial 7.
