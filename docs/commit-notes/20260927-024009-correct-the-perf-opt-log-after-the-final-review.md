# misc: Correct the perf-opt log after the final review

- **Date:** 2026-09-27 02:40   ·   **Branch:** feat/opt

## Goal
Make the perf-opt log claim only what the measurements support, and record
the Trial 5b determinism check.

## Summary of changes
- Noise: the same Trial 4 bfd binary run twice varied by 0.95-4.06%, so
  effects under ~4% are not demonstrated. Baseline start loads disclosed.
- Trial 5a: "no measurable effect, not adopted" (it passes against the first
  bfd run and fails against the rerun).
- Trial 5b: a rerun of the same v3 binary is bit-identical to its first run,
  so the result change is deterministic and codegen-target dependent.
- Profile: call graphs and IBS were not collected; the iTLB row counts only
  page walks; the executeInsts .cold split exists without PGO too.
- Cluster: stats IPC is 0.828 (0.817 is virt_run's own figure); kratos6 row
  wording; "separate physical cores"; the 1.65x spans two jobs; RUNPATH
  order; the portability claim is limited to the one checkpoint tested.
- Trial 6 records the user's PGO-for-bulk-runs decision; the summary names
  the adopted local and cluster binaries.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — accuracy corrections and the Trial 5b rerun.
