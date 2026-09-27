# misc: Log perf-opt trial 9 (IQ and IEW scans)

- **Date:** 2026-09-27 05:11   ·   **Branch:** feat/opt

## Goal
Record a rejected part-2 trial and keep its patch.

## Summary of changes
Trial 9 trimmed five per-cycle scans in the IQ and IEW (early-exit isFull,
pending FU-pool list, hasReadyInsts from listOrder, integer ghost sum, dead
IssueStruct::insts). Stats bit-identical, but only 2/5 checkpoints faster:
the gain is below the run-to-run noise. The source change is not kept.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — Trial 9 entry.
- `docs/perf-opt/patches/t09-iq-iew-scans.patch` — the rejected change.
