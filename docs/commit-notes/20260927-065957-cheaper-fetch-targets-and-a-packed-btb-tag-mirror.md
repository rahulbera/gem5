# cpu: Cheaper fetch targets and a packed BTB tag mirror

- **Date:** 2026-09-27 06:59   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 18: front-end bundle.

## Summary of changes
- `FTQ::insert()` takes its FetchTargetPtr by reference; squash loops iterate
  by reference (fewer atomic shared_ptr refcount pairs).
- BAC reuses a per-thread next-PC object (`nextFTPC`) instead of cloning a
  PC per fetch target, and fills the history target without a temporary
  clone.
- `SimpleBTB` keeps `tagMirror` (one valid|tid|tag word per slot), updated
  in update() and memInvalidate(); probe() compares against it and touches
  the entry only on a hit. Enabled when tags fit in 47 bits.
- Result: stats bit-identical on all five test checkpoints; 4/5 faster
  (llvm +5.6%) against a paired Trial 17 run.

## Files changed
- `src/cpu/o3/ftq.hh`, `src/cpu/o3/ftq.cc` — reference insert and loops.
- `src/cpu/o3/bac.hh`, `src/cpu/o3/bac.cc` — next-PC scratch; no temporary clone.
- `src/cpu/pred/btb_entry.hh` — setIndex(), getAssoc(), getTagMask().
- `src/cpu/pred/simple_btb.hh`, `src/cpu/pred/simple_btb.cc` — tag mirror.
- `docs/perf-opt/performance-opt-log.md` — Trial 18 entry.
