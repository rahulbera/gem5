# base,cpu-o3: Pool per-instruction objects with PooledNew

- **Date:** 2026-09-27 07:32   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 20: cheaper allocation for objects
created and destroyed per instruction or per branch.

## Summary of changes
- New `base/pooled_new.hh`: `PooledNew<T>` gives a class operator new/delete
  that recycle freed objects via a per-thread free list (other sizes fall
  back to the heap). Construction and destruction are unchanged.
- Applied to ArmISA::PCState, o3::DependencyEntry, BPredUnit::PredictorHistory
  and InstructionQueue::FUCompletion.
- Result: stats bit-identical on all five test checkpoints; 3/5 faster
  (+1.7% to +5.4%; llvm −2.3%, ns3 −3.2%) against a paired Trial 18 run.

## Files changed
- `src/base/pooled_new.hh` — new: PooledNew<T>.
- `src/arch/arm/pcstate.hh` — PCState derives from PooledNew.
- `src/cpu/o3/dep_graph.hh` — DependencyEntry derives from PooledNew.
- `src/cpu/o3/inst_queue.hh` — FUCompletion derives from PooledNew.
- `src/cpu/pred/bpred_unit.hh` — PredictorHistory derives from PooledNew.
- `docs/perf-opt/performance-opt-log.md` — Trial 20 entry.
