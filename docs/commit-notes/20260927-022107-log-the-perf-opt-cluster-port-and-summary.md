# misc: Log the perf-opt cluster port and summary

- **Date:** 2026-09-27 02:21   ·   **Branch:** feat/opt

## Goal
Record how the accepted build recipe was ported to kratos2 and validated
there, and close the log with a campaign summary.

## Summary of changes
Cluster builds (login node, g++-12 12.1) reproduce the local stats bit for
bit; results are portable across gcc 11.4/12.1/12.4 at the generic x86-64
target. The development recipe is +42% (SPEC) / +47% (agentic) over the old
cluster .opt; an agentic-trained PGO build adds +18-19%. Records the shipped
tcmalloc/protobuf libraries with an rpath, the safari-nexus1 exclusion, the
30-43% per-job slowdown from SMT-sibling sharing under CR_CPU allocation,
and a one-table summary of all trials.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — cluster port section and campaign summary.
