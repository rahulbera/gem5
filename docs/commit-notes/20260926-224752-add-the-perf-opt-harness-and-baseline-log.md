# util,misc: Add the perf-opt harness and baseline log

- **Date:** 2026-09-26 22:47   ·   **Branch:** feat/opt

## Goal
Give the gem5 speed campaign a reproducible way to build a configuration,
run the five-checkpoint test suite, check stats bit-identity and tabulate
KIPS, and record the baseline every trial is judged against.

## Summary of changes
Adds util/perf-opt/: seeded checkpoint selection (suite.json, seed
20260926), a resumable copy script, build.sh (g++-12, system Python,
records the exact build command), run_suite.sh (10M+30M fs_run.py runs,
one per pinned physical core), stats_gate.py (bit-identity gate and
nondeterminism finder) and kips_table.py (per-trial KIPS table and
verdict), with unit tests (23 pass). Starts
docs/perf-opt/performance-opt-log.md with the preamble and the baseline:
two baseline runs are bit-identical on all stats, run-to-run KIPS noise
is 0.36-1.57%, baseline KIPS 281-545. Also adds the implementation plan.

## Files changed
- `util/perf-opt/select_checkpoints.py`, `suite.json` — seeded draw of 5 test + 3 PGO-training checkpoints.
- `util/perf-opt/fetch_suite.sh` — copies the suite and restore resources from kratos2 and verifies them.
- `util/perf-opt/build.sh` — builds one configuration with the campaign toolchain and records how.
- `util/perf-opt/run_suite.sh` — runs one trial of the suite, pinned, with a stall timeout.
- `util/perf-opt/stats_gate.py` — bit-identity gate and nondeterministic-stat finder.
- `util/perf-opt/kips_table.py` — KIPS comparison table and accept/reject verdict.
- `util/perf-opt/test_*.py` — unit tests for the three Python tools.
- `docs/perf-opt/performance-opt-log.md` — log preamble and baseline section.
- `docs/perf-opt/nondeterministic-stats.txt` — gate exclusion list (empty: baseline is deterministic).
- `docs/superpowers/plans/2026-09-26-perf-opt.md` — the implementation plan.
