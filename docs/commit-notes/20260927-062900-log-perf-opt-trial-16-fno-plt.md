# misc: Log perf-opt trial 16 (-fno-plt)

- **Date:** 2026-09-27 06:29   ·   **Branch:** feat/opt

## Goal
Record a rejected part-2 build-flag trial.

## Summary of changes
Trial 16 compiled with -fno-plt (241,734 PLT calls down to 1, .text
+400 KB). Stats bit-identical, but all checkpoints within ±1.1% and only
2/5 faster: no effect. The flag is not added to the recipe.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — Trial 16 entry.
