# gem5 simulation-speed campaign, part 1: build and host levers

- **Date:** 2026-09-26
- **Branch:** `feat/opt`, cut from `rbdev` at `8d0dafc8154f039cffa0924fc0e25a9fdabb39ba`
- **Log:** `docs/perf-opt/performance-opt-log.md`

## Goal

Make our gem5 fork simulate faster before the bulk agentic experiments on the kratos2 cluster.
Stats must not change: every optimization must reproduce the baseline stats bit for bit. This
spec covers part 1 of the campaign. That is a measurement harness, a ladder of build- and
host-level levers tried in a fixed order, and a hotspot profile of the result. Part 2, the
code-level fixes to the hotspots that profile finds, gets its own spec. It uses the same gate
and the same log.

## What we already know

The pre-campaign investigation (2026-09-26) found the following.

- **Unused modules:** nothing from a module that is compiled in but not instantiated runs per
  cycle or per event. Earlier local runs of 190 SPEC checkpoints showed ALL and ARM builds at
  the same speed. Those two binaries differ by commit, so that comparison is suggestive, not
  proof.
- **Hot costs** are in modules we use:
  - the decoupled front end and the FDP prefetcher;
  - set-associative lookups that return a heap-allocated vector;
  - the L1D path with the stride prefetcher;
  - TAGE-SC-L folded histories;
  - per-instruction heap churn.
- **Untried build levers:** `.fast`, tcmalloc, `ext/` at -O0 today, LTO, `-march`, PGO.
- **Constraints:**
  - GCC emits FMA under `-march` levels that have it (x86-64-v3 and up), which can change
    host-FP results.
  - Local builds cannot run on the cluster (glibc 2.39 vs 2.35).
  - PGO data is tied to the exact GCC version.
  - kratos2 compute nodes have no tcmalloc.
  - One suspected source of run-to-run nondeterminism: TAGE `BranchInfo` arrays allocated
    without initialization.

## Decisions

| Topic | Decision |
|---|---|
| Workloads | SPEC26 checkpoints for local development; agentic checkpoints are the eventual target, validated on kratos2 |
| Scope | Part 1 = harness + build/host ladder + profile. Code-level fixes = part 2 |
| Correctness | Bit-identical non-host stats against the baseline, on every test checkpoint |
| Metric | gem5's own speed: region `hostInstRate` from `stats.txt`, reported as KIPS |
| Noise control | `taskset` pinning only; no clock or SMT changes |
| Toolchain | g++-12 locally (apt) and on the kratos2 login node |
| Adoption rule | Stats identical, and a majority (≥3 of 5) of test checkpoints faster |
| Logging | Every trial is logged, accepted or rejected |

## 1. Environment

- **Compiler:** g++-12 / gcc-12 from apt, passed as `CC=gcc-12 CXX=g++-12`.
- **Python and SCons:** system Python 3.12 with `python3-dev`, and apt `scons`. Miniconda stays
  off PATH, so no binary picks up miniconda's libstdc++ through RUNPATH.
- **Build configuration:** every build dir is configured from `build_opts/ARM` with
  `scons defconfig`. Auto-detected optional libraries stay whatever this box provides, the
  same for every build.
- **tcmalloc:** `libgoogle-perftools-dev` is installed only when the tcmalloc trial starts.
  Every build before that trial passes `--without-tcmalloc`, so none of them can silently
  pick it up.

## 2. Test suite

- **Five test checkpoints**, drawn at random from the 190 SPEC26 SimPoint checkpoints:
  - Source: the rows of `gem5-infra/ckpt-tools/simpoint/manifests/checkpoints.json`.
  - Method: `random.Random(20260926).sample(rows, 5)`.
  - The seed and the resulting list go in the log preamble.
- **Three PGO-training checkpoints**, drawn from the remaining 185 with the same generator,
  continuing the sequence. They are never used for evaluation.
- **Copying:** from `kratos2:/home/rahbera/tracezoo/gem5/fs_ckpts/spec26/` into the old local
  layout:
  - checkpoints go to `/home/rbera/work/tracezoo/gem5/fs_ckpts/<wl>/<inv>/cpt.<wl>.<inv>.<sp>/`;
  - `restore_resources/` goes to `/home/rbera/work/tracezoo/gem5/restore_resources/`, copied
    with `rsync -aSz` so the disk image stays sparse.

## 3. Run protocol

- **Command:** one trial runs all five test checkpoints through `configs/garfield/arm/fs_run.py`:

  ```
  GEM5_RESOURCE_DIR=/home/rbera/work/tracezoo/gem5/restore_resources \
  taskset -c <core> <gem5 binary> --outdir=<out> configs/garfield/arm/fs_run.py \
      --restore-dir <cpt dir> --benchmark <wl> --inv <inv> \
      --disk-img /home/rbera/work/tracezoo/gem5/restore_resources/spec_shared_root.img \
      --mem-size 16GiB --warmup-insts 10000000 --detailed-insts 30000000
  ```

- **Pinning:**
  - The five runs go concurrently, one per physical core, on logical CPUs 1–5.
  - Their SMT siblings, 17–21, stay idle.
  - Nothing else heavy runs during a trial. Builds never overlap runs.
- **Stall guard:** every run has a 30-minute wall-clock timeout; a normal run takes a few
  minutes. A timeout or non-zero exit fails the trial for that checkpoint.
- **Outputs:** `/home/rbera/work/agentic-cpu/perf-opt-runs/<trial>/<checkpoint>/`, outside the
  repo.
- **KIPS:** `hostInstRate / 1000` from the region dump in `stats.txt`. `fs_run.py` dumps the
  region stats only.

## 4. Gate

- **Baseline:** `build/ARM/gem5.opt` at the start commit, built with g++-12 and
  `--without-tcmalloc`. Its suite runs **twice**.
  - Any non-host stat that differs between the two runs is nondeterministic. It is reported
    in the preamble and excluded from the gate by name. No other stat is excluded.
  - The per-checkpoint KIPS difference between the two runs is the **noise figure** for that
    checkpoint. It is reported, not used to veto.
- **Identity:** each trial's `stats.txt` is compared with the baseline's using the
  existing rule from the EVES work:
  - lines starting with `host` or `simFreq`, blank lines, and separators are ignored;
  - every other baseline line must appear verbatim;
  - new stats must be zero.
- **Speed:** KIPS is compared with the **last accepted** configuration, so the ladder is
  cumulative. The table also shows cumulative change against the baseline.
- **Verdict:** accepted if and only if all five checkpoints are stats-identical **and** at
  least three of the five are faster than the last accepted configuration. Gains smaller than
  the noise figure are marked "within noise" in the table, but the rule stays as stated.

## 5. Ladder

Each lever gets its own build dir and builds on the last accepted configuration. A
rejected lever is left out of later trials.

| # | Trial | Mechanism |
|---|---|---|
| 1 | `.fast` | `gem5.fast` variant: `NDEBUG`, `TRACING_ON=0`, no `-g` |
| 2 | tcmalloc | Install `libgoogle-perftools-dev`. Let SCons link `tcmalloc_minimal`. Confirm with `ldd` |
| 3 | `ext/` optimized | `CCFLAGS_EXTRA=-O3` so `ext/` (DRAMPower etc.) is no longer built at -O0. `src/` is unaffected because the variant's `-O3` comes later |
| 4 | LTO | `--with-lto` (GCC). The same trial compares linkers bfd, gold and mold (apt), recording link time and KIPS |
| 5a | `-march=x86-64-v2` | Via `CCFLAGS_EXTRA`/`LINKFLAGS_EXTRA` |
| 5b | `-march=x86-64-v3 -ffp-contract=off` | AVX/AVX2 without FMA contraction. Adopted only if every kratos2 node class supports AVX2 (one tiny `lscpu` probe per node class) |
| 6 | PGO | `-fprofile-generate` build, trained on the three training checkpoints (10M+30M), then `-fprofile-use -fprofile-partial-training -Wno-missing-profile` |

`CCFLAGS_EXTRA` and `LINKFLAGS_EXTRA` are environment variables that are not sticky. Every
scons call for a build dir exports the same values. The exact build command for each dir is
recorded in the log.

## 6. Profiling (after the ladder)

- **Target:** the final accepted configuration, rebuilt with `-g` added. `-g` does not change
  generated code.
- **Workload:** at least two test checkpoints.
- **perf:** `perf record --call-graph dwarf` for hotspots and call paths; `perf stat` for IPC,
  instruction-cache and iTLB misses, and front-end-boundness.
- **AMD uProf:** timer sampling plus IBS, as a cross-check.
- **Output:** a ranked hotspot list with self and total time and source locations, plus a
  front-end vs back-end characterization. It goes in the log and becomes the input to the
  part-2 spec.

## 7. Port to kratos2

- **Build:** rebuild the accepted recipe on the kratos2 login node with g++-12.
- **tcmalloc:** link it statically, or ship its `.so` next to the binary, because compute nodes
  lack it.
- **PGO:** PGO data from this box cannot be reused (GCC 12.4 here, 12.3 there). Training runs
  on the cluster go through Slurm, following the cluster-etiquette rules.
- **Check:** confirm the gate and the speedup on one SPEC checkpoint and one agentic checkpoint.

## 8. Log format

`docs/perf-opt/performance-opt-log.md` is committed on `feat/opt` after every trial, with no
AI trailers.

- **Preamble:**
  - start hash;
  - host and toolchain;
  - test-suite seed and checkpoints;
  - training checkpoints;
  - run protocol;
  - gate rule;
  - nondeterministic stats;
  - noise figure;
  - baseline KIPS.
- **One subsection per trial, in this order:**
  - heading;
  - one- or two-line summary;
  - key idea;
  - files and flags targeted, with the exact build command;
  - KIPS table: checkpoint | previous KIPS | new KIPS | change % | direction | cumulative vs
    baseline | stats identical;
  - verdict (accepted or rejected, and on what grounds).
- **Rejected and negative trials** are logged as fully as accepted ones.

## Out of scope

- Model-code changes (part 2).
- Changing simulated behaviour, including `have_large_asid_64` for the SPEC26 SimPoint checkpoints.
- Host clock and SMT tuning.
- Multi-core or parallel gem5.
