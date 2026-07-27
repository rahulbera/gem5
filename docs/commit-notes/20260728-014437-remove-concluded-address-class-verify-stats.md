# cpu-o3: Remove concluded address-class verify stats

- **Date:** 2026-07-28 01:44   ·   **Branch:** rbdev

## Goal
Post-closure stats hygiene: a three-way audit (code sites x empirical
activity across th14/gate/ledger runs x provenance in the research log)
found exactly one family of always-on residue from the concluded
criticality investigations. Remove it; keep everything with a live role.

## Summary of changes
Deleted the address-stability verify classifier: lvWrongAddrChanged,
lvWrongAddrSame, lvCorrectAddrChanged, lvCorrectAddrSame and their
vfNoteLastValueAddrClass wrapper + both LSQ call sites. These four scalars
classified every consumed last-value verify unconditionally, and their sole
purpose was the (concluded, reported) Condition-1 address-instability probe
-- the 80.2%/23.7% split is recorded in the research log. The gate's
decision plumbing is untouched: cellAddrChanged -> vf_ac -> trainVerify
still feeds the strike test when the stability gate is enabled, and the
gate/ledger keep their knob-gated observability (lvStrikes,
lvProbationSuppressed, lvEarningSpared).

Audit verdicts on everything else, for the record: no dead stats (all 41
had live increment paths); the memory-service-level family (loadLevelAll,
vfConsumedLevelCorrect/Wrong, vfWrongFlushedByLevel) is kept deliberately
as permanent observability (Rahul's call); vfConfidentUnconsumed is kept as
support for the surviving --mrn-vf-no-* ablation flags (zero only because
all scanned configs run full modes); core accounting/coverage/structural
stats untouched.

Verified: clean build, 20/20 unit tests, bit-identical on
750.sealcrypto_r.0.2 vs the th14 reference (3,566 shared stats, 0 differ).

## Files changed
- `src/cpu/o3/mem_rename_predictor.hh` — drop the four stat decls and the
  vfNoteLastValueAddrClass wrapper.
- `src/cpu/o3/mem_rename_predictor_sim.cc` — drop the four ADD_STATs.
- `src/cpu/o3/lsq_unit.cc` — drop both classifier call sites (vf_lv/vf_ac
  stay: they feed the stability gate's strike test).
