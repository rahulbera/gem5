# misc: Log perf-opt overall result and cluster port

- **Date:** 2026-09-27 13:13   ·   **Branch:** feat/opt

## Goal
Record the end-to-end local result of both campaign parts and the kratos2
port of part 2.

## Summary of changes
- Overall, local, back to back against the original gem5.opt: development
  build 1.55x (geomean), PGO build 1.83x; stats identical except the
  assert-inflated numMiscRegReads.
- kratos2: feat/opt built there (ARM_rel2, agentic-trained ARM_pgo2).
  Against part 1's cluster binaries, on separate physical cores:
  development +14.8% (llvm) / +13.3% (gin), PGO +16.8% / +16.7%; all
  bit-identical. compare-two.sbatch now avoids SMT-sibling placement.

## Files changed
- `docs/perf-opt/performance-opt-log.md` — overall result and cluster port sections.
