# cpu-o3: Cheaper LSQ request access and allocation

- **Date:** 2026-09-27 06:04   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 14: cut shared_ptr traffic and
allocations per memory access in the LSQ.

## Summary of changes
- `LSQRequest::req()` and `mainReq()` return `const RequestPtr &` instead
  of a copy (an atomic refcount pair per call).
- `LSQRequest::addReq()` uses `std::make_shared<Request>` (one allocation
  instead of two); the local accessor lambda captures the raw pointer.
- `LSQUnit::read()` reads the load's address range and LLSC flag once
  before the store-forwarding scan.
- Result: stats bit-identical on all five test checkpoints; +0.7% to +5.5%
  KIPS against a paired Trial 13 run.

## Files changed
- `src/cpu/o3/lsq.hh` — reference-returning req()/mainReq().
- `src/cpu/o3/lsq.cc` — SplitDataRequest::mainReq(); make_shared in addReq.
- `src/cpu/o3/lsq_unit.cc` — hoisted load-side values in read().
- `docs/perf-opt/performance-opt-log.md` — Trial 14 entry.
