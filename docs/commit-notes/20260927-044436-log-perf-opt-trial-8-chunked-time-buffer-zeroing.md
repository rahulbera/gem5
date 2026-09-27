# misc: Log perf-opt trial 8 (chunked time-buffer zeroing)

- **Date:** 2026-09-27 04:44   ·   **Branch:** feat/opt

## Goal
Record a rejected part-2 trial and keep its patch for posterity.

## Summary of changes
Trial 8 zeroed TimeBuffer slots in 64-byte pieces to avoid `rep stosq`.
Stats stayed bit-identical, but only 2/5 checkpoints got faster: the
placement-new constructors after the reset then compiled to `rep stosq`
themselves. The source change is not kept; its patch is.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — Trial 8 entry.
- `docs/perf-opt/patches/t08-timebuf-chunked-zero.patch` — the rejected change.
