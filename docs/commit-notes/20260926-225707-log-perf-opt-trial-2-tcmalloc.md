# misc: Log perf-opt trial 2 (tcmalloc)

- **Date:** 2026-09-26 22:57   ·   **Branch:** feat/opt

## Goal
Record the second ladder trial: linking tcmalloc_minimal.

## Summary of changes
tcmalloc on top of .fast is 21.2-23.0% faster on all five test checkpoints
with stats bit-identical to the .fast run (cumulative +29.0-32.0% over the
.opt baseline). Accepted. Notes that the kratos2 port must ship or
statically link tcmalloc, since compute nodes lack it.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — trial 2 subsection.
