# misc,util: Log perf-opt trial 1 (.fast)

- **Date:** 2026-09-26 22:56   ·   **Branch:** feat/opt

## Goal
Record the first ladder trial: the .fast build variant.

## Summary of changes
.fast is 6.4-8.6% faster than .opt on all five test checkpoints. Its only
stats difference is executeStats0.numMiscRegReads, inflated in .opt by the
debug-only assert(checkInterrupts()) in ArmISA::Interrupts::getInterrupt
(4 counted reads per evaluation). Accepted by user ruling; from trial 2 on
the identity reference is the .fast run. kips_table.py gains --ident so the
identity reference can differ from the KIPS baseline (new unit test).

## Files changed
- `docs/perf-opt/performance-opt-log.md` — trial 1 subsection; identity-reference note in the gate section.
- `util/perf-opt/kips_table.py` — new --ident option (identity reference, default the first --base).
- `util/perf-opt/test_kips_table.py` — test for a separate identity reference.
