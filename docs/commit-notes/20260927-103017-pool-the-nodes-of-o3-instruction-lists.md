# base,cpu-o3: Pool the nodes of O3 instruction lists

- **Date:** 2026-09-27 10:30   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 21: stop allocating a list node each
time an instruction enters one of the pipeline's lists.

## Summary of changes
- `PooledAllocator<T>` in `base/pooled_new.hh`: a standard allocator that
  recycles single-element allocations through a per-thread free list.
- `o3::DynInstList` (std::list<DynInstPtr> with that allocator) replaces
  std::list<DynInstPtr> in the CPU, ROB, IQ and memory-dependence unit.
- `CommitCPUStats::updateComCtrlStats` takes `const StaticInstPtr &`.
- Result: stats bit-identical on all five test checkpoints; 4/5 faster
  (+2.2% to +3.8%) against a paired Trial 20 run.

## Files changed
- `src/base/pooled_new.hh` — PooledAllocator<T>.
- `src/cpu/o3/dyn_inst_ptr.hh` — DynInstList alias.
- `src/cpu/o3/dyn_inst.hh`, `cpu.hh`, `rob.hh`, `inst_queue.hh`, `mem_dep_unit.hh` — use it.
- `src/cpu/base.hh`, `src/cpu/base.cc` — updateComCtrlStats by reference.
- `docs/perf-opt/performance-opt-log.md` — Trial 21 entry.
