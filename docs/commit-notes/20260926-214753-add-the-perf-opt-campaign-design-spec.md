# misc: Add the perf-opt campaign design spec

- **Date:** 2026-09-26 21:47   ·   **Branch:** feat/opt

## Goal
Record the agreed design for making gem5 simulate faster before the bulk
agentic experiments, without changing any simulation result.

## Summary of changes
Adds the part-1 design spec: a local measurement harness on five random
SPEC26 checkpoints (10M warmup + 30M detailed, taskset-pinned), a
bit-identical stats gate against a twice-run baseline, and a fixed ladder
of build levers (.fast, tcmalloc, optimized ext/, LTO with a linker
comparison, -march, PGO) followed by a perf/uProf hotspot profile and a
port to kratos2. Every trial, accepted or rejected, is to be logged in
docs/perf-opt/performance-opt-log.md.

## Files changed
- `docs/superpowers/specs/2026-09-26-perf-opt-design.md` — the design spec.
