# cpu-o3: Add execute-at-fetch run-ahead engine

- **Date:** 2026-06-28 12:51   ·   **Branch:** rbdev

## Goal
Build the execute-at-fetch substrate: a mechanism where every correct-path
instruction's ground truth (next-PC, destination register values, memory
address/value, fault) is known at fetch and validated against the O3 core's
real execution at commit. This is v1 — metadata-only, O3 behavior unchanged —
groundwork for future oracle predictors (value / branch / memory-dependence
headroom studies, Scarab-inspired). Scope: SE, single-core, ARM.

## Summary of changes
Adds a `RunAheadEngine` — a non-perturbing `BaseCPU` side-car modeled on
`CheckerCPU` (`is_checker=true`) — that runs one instruction ahead of the
core, executing functionally over a copy-on-write store overlay seeded from
the SE image (it never writes real memory), with LL/SC monitor and AMO (CAS)
support. Each instruction's `OracleInfo` is aligned to the O3 `DynInst` at
fetch via a PC-keyed FIFO and validated at commit; a mismatch panics — the
correctness keystone. The shadow thread resyncs from the core at startup and
after each syscall barrier. Validation is opt-in (`--oracle-validate`) and off
by default, so normal O3 runs are unaffected (core stats byte-identical).

Status: validates ~30K instructions of libc startup byte-exact, including
syscalls, LL/SC, and AMO atomics. One known control-flow divergence remains
in libc's exception-frame code (~sn:30K) — under investigation, likely a
squash path not yet covered by the FIFO rewind.

## Files changed
- `src/cpu/o3/run_ahead_engine.{hh,cc}` — the engine: `ExecContext` shadow
  execution, `produceNext`, and the fetch/commit/squash/syscall hooks.
- `src/cpu/o3/run_ahead_overlay.{hh,cc}` + `.test.cc` — copy-on-write store
  overlay (read base-then-patch, write, drop); 5 unit tests.
- `src/cpu/o3/run_ahead_fifo.{hh,cc}` + `.test.cc` — PC-aligned `OracleInfo`
  FIFO (push / match-consume / rewind / release); 5 unit tests.
- `src/cpu/o3/RunAheadEngine.py` — SimObject params (enable, validateOracle,
  injectOracleFault, capacity) + checker-style classmethods.
- `src/cpu/o3/oracle_info.cc` — richer `dump()` (dest hex values + mem addr).
- `src/cpu/o3/BaseO3CPU.py` — `runAheadEngine` param + `createThreads` cascade.
- `src/cpu/o3/cpu.{hh,cc}` — engine member, wiring, startup resync.
- `src/cpu/o3/fetch.cc` — attach `OracleInfo` to each `DynInst` at `buildInst`.
- `src/cpu/o3/commit.cc` — `validateAtCommit` + `onSyscallCommit` hooks.
- `src/cpu/o3/iew.cc` — dcache-port wiring + squash-rewind hooks.
- `src/cpu/o3/dyn_inst.hh` — `OracleInfo` handle on the `DynInst`.
- `src/cpu/o3/SConscript` — register sources, GTests, and the `Oracle` flag.
- `src/arch/arm/ArmCPU.py` — `ArmRunAheadEngine` ISA flavoring.
- `configs/garfield/arm/se_run.py` — `--oracle` / `--oracle-validate` switch.
