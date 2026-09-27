# util: Fail perf-opt gates on missing or partial trials

- **Date:** 2026-09-27 02:39   ·   **Branch:** feat/opt

## Goal
Close two holes found in the final review of the perf-opt harness: a trial
missing some checkpoints, or a mistyped baseline, could pass the gates.

## Summary of changes
- `kips_table.rows` now walks the union of the reference's and the trial's
  checkpoints. A checkpoint without stats appears as a MISSING row, and
  `verdict` rejects the trial ("stats missing on N/M") instead of judging
  it on the checkpoints that happened to finish.
- `stats_gate.gate` fails when the baseline dir has no stats at all, and
  fails on checkpoints present on only one side, instead of printing nothing
  and exiting 0.
- New tests: `PartialTrialTest` and `GateTest` (suite 28/28).

## Files changed
- `util/perf-opt/kips_table.py` — list and reject checkpoints missing from a trial.
- `util/perf-opt/stats_gate.py` — fail on an empty baseline or a one-sided checkpoint.
- `util/perf-opt/test_kips_table.py` — partial-trial test.
- `util/perf-opt/test_stats_gate.py` — gate tests for missing, one-sided and matching trials.
