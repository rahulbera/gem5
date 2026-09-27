# misc,util: Log the perf-opt profile of the accepted build

- **Date:** 2026-09-27 00:33   ·   **Branch:** feat/opt

## Goal
Characterize where the adopted (PGO) gem5 build still spends host time,
as the input to the code-level part of the campaign.

## Summary of changes
Adds util/perf-opt/profile_region.sh, which attaches a profiler only to
the measured region of one fs_run.py restore (triggered by the
"(measured)" marker). The log gains a profile section: gem5 is ~46%
front-end bound on Zen 5 (iTLB negligible); the hotspots are per-
instruction heap churn (13-15%), IQ/IEW scheduling (15-19%) and TAGE-SC-L
(8.5-13.7%); perf and uProf agree on the top ten. It ends with a ranked
hand-off list for part 2. Also adds a dated correction to trial 3: with
ext at -O0 DRAMPower is only 0.14% of samples, so trial 3's gains were
layout or measurement noise, not DRAMPower.

## Files changed
- `util/perf-opt/profile_region.sh` — profile only the detailed region of one restore.
- `docs/perf-opt/performance-opt-log.md` — profile section, part-2 hand-off, trial 3 correction.
