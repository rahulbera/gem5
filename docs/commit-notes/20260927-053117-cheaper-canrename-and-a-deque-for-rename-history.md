# cpu-o3: Cheaper canRename and a deque for rename history

- **Date:** 2026-09-27 05:31   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 11: cut per-instruction work and
allocations in the rename stage.

## Summary of changes
- `UnifiedRenameMap::canRename` takes `const DynInstPtr &` and skips
  register classes with no destinations (same answer).
- `Rename::historyBuffer` is a `std::deque` instead of a `std::list`;
  `doSquash` and `removeFromHistory` now work from the ends (front for
  squashes, back for commits), processing the same entries in the same
  order without the list-only iterator tricks.
- Result: stats bit-identical on all five test checkpoints; 4/5 faster
  (+1.6% to +2.8%) against a paired Trial 10 run.

## Files changed
- `src/cpu/o3/rename_map.hh`, `src/cpu/o3/rename_map.cc` — canRename.
- `src/cpu/o3/rename.hh` — history buffer is a deque.
- `src/cpu/o3/rename.cc` — end-based squash and commit loops.
- `docs/perf-opt/performance-opt-log.md` — Trial 11 entry.
