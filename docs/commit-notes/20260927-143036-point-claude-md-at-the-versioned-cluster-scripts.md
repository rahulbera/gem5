# misc: Point CLAUDE.md at the versioned cluster scripts

- **Date:** 2026-09-27 14:30   ·   **Branch:** feat/opt

## Goal
The kratos2 build and Slurm scripts are now versioned in the gem5-infra
repo (scripts/); make CLAUDE.md point there instead of only at the
cluster-local copy.

## Summary of changes
The Recommended builds section names gem5-infra's scripts/build-p2.sh (and
the restore-one, compare-two and pgo-train Slurm scripts) as the versioned
copies, with /home/rahbera/agentic-cpu/slurm/ as the working copy.

## Files changed
- `CLAUDE.md` — kratos2 build bullet points at gem5-infra/scripts.
