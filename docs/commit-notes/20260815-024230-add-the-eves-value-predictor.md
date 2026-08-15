# cpu-o3,configs: Add the EVES value predictor (E-VTAGE + E-Stride)

- **Date:** 2026-08-15 02:42   ·   **Branch:** vp

## Goal
Task 3 of the E-Stride/EVES design: the `EvesVP` SimObject composing
the already-landed `EStrideTable` core (Task 2) and the owned
`EVtageTables` instance behind the CVP-1 write-order arbitration
(`evesArbitrate()`, eves_arbiter.hh), plus the CLI/config plumbing and
the guard-capable `usesInflightCounts()`/`notifyRenamedInst` hooks'
first real coverage.

## Summary of changes
`eves.{hh,cc}`: `EvesVP` owns an `EStrideTable` and an `EVtageTables`
(same injected-RNG idiom as `EVtageVP`/`VtageVP`), overrides
`trainsAtCommit()`/`usesHistory()`/`usesInflightCounts()` all true.
Three token flag bits (63/62/61: stridePredicted, vtageConfident,
deliveredByVtage) sit above `EVtageTables`'s own packed token; the
ctor `fatal_if`s the token geometry (`ceilLog2` recompute of the
index-field width) against a 61-bit budget. `predictImpl` runs the
stride lookup then the VTAGE lookup then `evesArbitrate()`, exactly
as the CVP-1 source's write-order mechanism (stride first, VTAGE
overwrites even below confidence in verbatim mode). `trainImpl` trains
VTAGE then E-Stride, folding stride outcomes into a 15-stat surface.
`correctiveResetImpl` routes the SafeStride penalty / lastWrongMark
reset / VTAGE corrective punish off the three flag bits per spec S6 --
a stride-supplied wrong gets no VTAGE punish and reports live (no
staleness question asked). `ValuePredictor.py` duplicates all ten
`EVtageVP` params verbatim (declared drift risk, spec S2) plus
`vtageOverwriteRequiresConfidence`; `sim_opts.py` adds the `eves`
`--use-vp` choice and `--eves-vtage-overwrite-requires-confidence`,
sharing `--evtage-burst-guard`/`-conf-threshold`/`-hist-lengths` with
the `evtage` arm (spec S2 amended in this commit to match: no separate
`--eves-burst-guard`). `evtage.{hh,cc}`'s comment-only rewording (the
rename feed exists but E-VTAGE's own guard stays locked off) was
already discharged in an earlier commit -- untouched here.

Gates: full build clean, `--help` runs; 5/5 GTest binaries green
(21+4+15+32+54); four boot smokes (vpchase, all four `--use-vp eves`
variants) rc=0, `predictionsMade > 0`, inflight closure identity
exact at drained exit. Directed routing smokes (branchsort/vpalu,
`--evtage-conf-threshold 1`): `deliveredWrongByVtage ==
vtageCorrectivePunishes + correctiveResetStale` and `predictionsWrong
== deliveredWrongByVtage + deliveredWrongByStride` hold exactly on
every run, including one crafted with
`--eves-vtage-overwrite-requires-confidence` to force real
stride-supplied wrongs (561 of them, correctly routed with zero VTAGE
punishes). `predictionsCorrect ==
deliveredCorrectByVtage+deliveredCorrectByStride` and the inflight
closure show a small residual (single digits to a few dozen, out of
populations up to 1.3M) on these hostile, fully-drained runs only;
root-caused via temporary per-seqNum instrumentation (reverted, not
part of this commit) to `m5_reset_stats()` at ROI_BEGIN (roi.h)
zeroing the gem5 Stat counters while a handful of already-renamed
instructions are still in flight across rename-to-commit or
verify-to-commit -- the underlying algorithmic state (the
`VpInflightCounted` flag, the per-key map) is untouched by the reset
and balances to exactly zero at true process exit. The boot smokes
(no reset straddling) show zero residual. Bit-identity re-gate 5/5
(`lvp`, `vtage`, `evtage`, `evtage_all`, `mrn_vp`) IDENTICAL modulo
new zero stats.

## Files changed
- `src/cpu/o3/vp/eves.hh`, `eves.cc` — the `EvesVP` SimObject: token
  flags, owned `EStrideTable`/`EVtageTables`, predict/train/corrective-
  reset routing, 15-stat surface (`EvesStats`).
- `src/cpu/o3/vp/ValuePredictor.py`, `SConscript` — `EvesVP` params
  (duplicated E-VTAGE geometry + `vtageOverwriteRequiresConfidence`)
  + build registration.
- `configs/garfield/arm/sim_opts.py` — `eves` `--use-vp` choice,
  `--eves-vtage-overwrite-requires-confidence`, the `make_vp` arm.
- `docs/superpowers/specs/2026-08-14-estride-design.md` — S2 amended:
  the shared `--evtage-burst-guard` replaces the planned separate
  `--eves-burst-guard` flag.
