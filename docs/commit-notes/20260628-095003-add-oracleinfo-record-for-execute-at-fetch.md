# cpu-o3: Add OracleInfo record for execute-at-fetch

- **Date:** 2026-06-28 09:50   ·   **Branch:** rbdev

## Goal
First building block of the execute-at-fetch substrate (run-ahead engine): a
per-instruction ground-truth record that the engine will produce and attach to
each correct-path DynInst at fetch, and that the commit stage will validate
against the real execution. Implemented test-first.

## Summary of changes
Add the `gem5::o3::OracleInfo` struct (true index, PC/next-PC, destination
register writes, memory eff-addr/value, fault) and `OracleRegResult` with an
order-sensitive `destsMatch()` comparator and a `dump()` for diagnostics, plus
GoogleTests. Design note: register identity is stored as a plain `int`
(a `RegClassType` value) and next-PC as `Addr`, so the record carries no
dependency on `cpu/reg_class.hh` — this keeps the unit test linkable without
gem5's debug-flag machinery; the engine/validator translate from real `RegId`s.

## Files changed
- `src/cpu/o3/oracle_info.hh` — `OracleInfo` / `OracleRegResult` definitions.
- `src/cpu/o3/oracle_info.cc` — `destsMatch()` and `dump()` implementations.
- `src/cpu/o3/oracle_info.test.cc` — GTests: dest match (equal / value / count /
  class mismatch) and dump (5 tests, all pass).
- `src/cpu/o3/SConscript` — register the source and the GTest.
