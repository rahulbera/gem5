# misc: Log perf-opt trial 15 (probe block hint)

- **Date:** 2026-09-27 06:19   ·   **Branch:** feat/opt

## Goal
Record a rejected part-2 trial and keep its patch.

## Summary of changes
Trial 15 passed the hit block to the prefetcher's probe so that
probeNotify could skip a second tag lookup. Stats bit-identical, but only
1/5 checkpoints faster: after Trial 13 the saved lookup is cheap. The
source change is not kept.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — Trial 15 entry.
- `docs/perf-opt/patches/t15-probe-hint.patch` — the rejected change.
