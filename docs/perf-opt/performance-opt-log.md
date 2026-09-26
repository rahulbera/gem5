# gem5 simulation-speed optimization log

This log records every optimization trial of the perf-opt campaign, whether it was
accepted or rejected. Negative results are kept as carefully as positive ones: they
tell the next person what not to try again. The design is in
`docs/superpowers/specs/2026-09-26-perf-opt-design.md`.

## Preamble

### Starting point
- **Branch:** `feat/opt`, cut from `rbdev` at `8d0dafc8154f039cffa0924fc0e25a9fdabb39ba`
  ("arm: restore QEMU-virt checkpoints with 16-bit ASIDs").
- **Simulator code:** every trial builds the same simulator sources. Only build flags,
  libraries and link steps change, so any stats difference is a real defect of the lever.

### Host and toolchain
- **Host:** AMD Ryzen AI MAX+ 395 (Zen 5, 16 cores / 32 threads, up to 5.19 GHz,
  2×32 MB L3), 60 GiB RAM, Ubuntu 24.04, kernel 7.0.0. Clocks and SMT are left at their
  defaults; the machine is otherwise lightly loaded.
- **Compiler:** g++-12 12.4.0 (`CC=gcc-12 CXX=g++-12`). The same major version is used
  on the kratos2 cluster, so results and recipes carry over.
- **Python and SCons:** system Python 3.12 (`python3-dev`) and SCons 4.5.2, with conda
  kept off PATH. Every binary links the system `libstdc++` and `libpython3.12`.
- **Build configuration:** every build dir is configured from `build_opts/ARM` and has
  the same auto-detected features: Ruby with CHI, SystemC, x86 KVM, capstone. No
  protobuf, no HDF5, no PNG.
- **Harness:** `util/perf-opt/`. `build.sh` builds a configuration and records the exact
  command; `run_suite.sh` runs one trial; `stats_gate.py` checks bit-identity;
  `kips_table.py` prints each trial's table and verdict.

### Test suite
- **Test checkpoints:** five SPEC CPU 2026 SimPoint checkpoints, drawn with
  `random.Random(20260926).sample(rows, 5)` from the 190 rows of
  `gem5-infra/ckpt-tools/simpoint/manifests/checkpoints.json`:
  `708.sqlite_r.0.3`, `706.stockfish_r.2.4`, `706.stockfish_r.1.1`, `753.ns3_r.2.0`,
  `723.llvm_r.1.0`. The draw happens to include two stockfish checkpoints.
- **PGO training checkpoints:** drawn next from the remaining 185 by the same
  generator, and never used for evaluation: `748.flightdm_r.2.2`, `721.gcc_r.2.0`,
  `767.nest_r.1.0`.
- **Selection record:** `util/perf-opt/suite.json`.

### Run protocol
- **Command:** each checkpoint runs `configs/garfield/arm/fs_run.py` with 10M warmup
  and 30M detailed instructions (`--warmup-insts 10000000 --detailed-insts 30000000`,
  `--mem-size 16GiB`) on the Neoverse V2 O3 model with default knobs.
- **Pinning:** the five runs go concurrently, each pinned with `taskset` to its own
  physical core (logical CPUs 1–5; their SMT siblings 17–21 stay idle). Builds never
  overlap runs.
- **KIPS:** the region's `hostInstRate` from `stats.txt`, divided by 1000.
  `fs_run.py` dumps only the region stats.

### Gate
- **Identity:** a trial's `stats.txt` must match the baseline on every checkpoint.
  - Lines starting with `host` or `simFreq`, blank lines and separators are ignored.
  - Every other baseline line must appear verbatim.
  - New stats must be zero.
  - Stats that differ between two identical baseline runs are listed in
    `docs/perf-opt/nondeterministic-stats.txt` and excluded by name.
- **Verdict:** a lever is **accepted** if and only if stats are identical on all five
  checkpoints **and** at least three of the five are faster than the last accepted
  configuration. The ladder is cumulative.
- **Noise:** the noise figure is the run-to-run KIPS spread of the two baseline runs.
  Gains inside it are marked "within noise" but still count.

## Baseline

- **Build:** `build/ARM/gem5.opt`, built in 595 s. The binary is 1126 MB, of which
  45.2 MB is `.text`. No tcmalloc.

  ```
  CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='' LINKFLAGS_EXTRA='' scons build/ARM/gem5.opt -j32 --ignore-style --without-tcmalloc
  ```

- **Two identical runs** (`baseline-a` 22:39, `baseline-b` 22:43, 2026-09-26) were
  **bit-identical on every stat of all five checkpoints**. No stat is nondeterministic
  in these windows, so `nondeterministic-stats.txt` excludes nothing.
- **Noise figure:** the run-to-run KIPS spread is **0.36–1.57%**.
- **Fixed cost per run:** wall time per run minus the region's `hostSeconds` is 50–105 s.
  That covers startup, resource lookup, restore and the 10M warmup. The detailed region
  is 55–108 s.

| Checkpoint | IPC | KIPS run a | KIPS run b | Baseline KIPS (mean) | Spread |
|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 2.282 | 522.9 | 521.0 | 521.9 | 0.36% |
| 706.stockfish_r.2.4 | 2.515 | 544.1 | 546.1 | 545.1 | 0.36% |
| 708.sqlite_r.0.3 | 1.808 | 379.7 | 374.4 | 377.1 | 1.41% |
| 723.llvm_r.1.0 | 1.101 | 282.7 | 278.5 | 280.6 | 1.50% |
| 753.ns3_r.2.0 | 1.259 | 308.8 | 304.0 | 306.4 | 1.57% |

## Trials

