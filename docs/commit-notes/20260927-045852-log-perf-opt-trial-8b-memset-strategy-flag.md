# misc: Log perf-opt trial 8b (memset strategy flag)

- **Date:** 2026-09-27 04:58   ·   **Branch:** feat/opt

## Goal
Record an accepted build-flag trial of part 2 and the new build recipe.

## Summary of changes
Trial 8b adds `-mmemset-strategy=vector_loop:2048:noalign,libcall:-1:noalign`
to CCFLAGS_EXTRA and LINKFLAGS_EXTRA. Fixed-size memsets up to 2 KB become
vector-store loops instead of `rep stosq` (0 left in CPU::tick, 36 in the
binary, from 3323). Stats bit-identical on 5/5; 4/5 checkpoints faster
against a paired Trial 7 run. No source change.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — Trial 8b entry and recipe.
