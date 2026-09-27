# mem-cache,arch-arm: Look up set-associative sets in place

- **Date:** 2026-09-27 05:55   ·   **Branch:** feat/opt

## Goal
Part 2 of the perf-opt campaign, Trial 13: stop copying a set's candidate
vector on every cache, TLB and predictor-table lookup.

## Summary of changes
- `IndexingPolicyTemplate::possibleEntriesInPlace()` (virtual, default
  nullptr) returns the set a policy stores, without copying; overridden by
  SetAssociative, TaggedSetAssociative, TLBSetAssociative and
  BTBSetAssociative.
- `BaseTags::findBlock` walks the set in place and computes the tag once
  (`TaggedEntry::matchTag`); AssociativeCache::findEntry/findVictim and
  ArmISA's TLB::Table::findEntry use the in-place set. Same entries, same
  order; the copying path remains for other policies.
- Result: stats bit-identical on all five test checkpoints; 3/5 faster
  (llvm +2.7%, sqlite +1.5%, ns3 +0.6%; stockfish −2.1%/−0.9%).

## Files changed
- `src/mem/cache/tags/indexing_policies/base.hh` — possibleEntriesInPlace().
- `src/mem/cache/tags/indexing_policies/set_associative.{hh,cc}` — override.
- `src/mem/cache/tags/tagged_entry.hh` — override; TaggedEntry::matchTag().
- `src/mem/cache/tags/base.cc` — in-place findBlock with the tag computed once.
- `src/base/cache/associative_cache.hh` — in-place findEntry/findVictim.
- `src/arch/arm/pagetable.hh`, `src/arch/arm/tlb.cc` — TLB override and lookup.
- `src/cpu/pred/btb_entry.hh` — BTB override.
- `docs/perf-opt/performance-opt-log.md` — Trial 13 entry.
