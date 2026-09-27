# cpu: Allocation-free BTB search for fetch targets

- **Date:** 2026-09-27 05:21   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 10: make the decoupled front end's
per-address BTB search cheap.

## Summary of changes
- `BTBSetAssociative::possibleEntries()` returns a set's entries by
  reference (`getPossibleEntries()` still returns a copy, via it).
- `BTBEntry::matchTag()` matches against a precomputed tag.
- `SimpleBTB::probe()` walks the set in place, computing the tag once;
  `valid()`, `getInst()` and `findEntry()` use it. Same entry, same way
  order, no replacement or stats update. `lookup()` is unchanged.
- Result: stats bit-identical on all five test checkpoints; +1.4% to +6.7%
  KIPS (llvm +6.7%) against a paired Trial 8b run.

## Files changed
- `src/cpu/pred/btb_entry.hh` — possibleEntries() by reference; matchTag().
- `src/cpu/pred/simple_btb.hh` — probe() and the cached set-associative policy.
- `src/cpu/pred/simple_btb.cc` — probe(); valid/getInst/findEntry use it.
- `docs/perf-opt/performance-opt-log.md` — Trial 10 entry.
