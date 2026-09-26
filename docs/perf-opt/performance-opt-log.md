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
- **Identity reference:** `baseline-a` for Trial 1. From Trial 2 on it is `trial1-fast`,
  following the user's ruling on Trial 1 (below).

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

### Trial 1 — `.fast` build variant

**Summary:** building `gem5.fast` instead of `gem5.opt` speeds up every test checkpoint
by 6.4–8.6%. Every stat is identical except one assert-inflated counter (explained
below). **Accepted.**

**Key idea:** `.opt` is `-O3 -g` with `TRACING_ON=1` and asserts enabled. `.fast` is `-O3`
with `NDEBUG` and `TRACING_ON=0`. That removes every `assert`/`gem5_assert`, the per-event
`debug::Event` check, the `DPRINTF` flag tests and the NDEBUG-only O3 instruction-count
bookkeeping from the hot paths (`src/SConscript:680-682`).

**Files and flags targeted:** build variant only; no source change. Objects are `.fo`, so
the `.opt` build in the same dir is untouched.

```
CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='' LINKFLAGS_EXTRA='' scons build/ARM/gem5.fast -j32 --ignore-style --without-tcmalloc
```

Built in 319 s. The binary is 94 MB (1126 MB for `.opt`, which carries debug info), with
42.4 MB of `.text` against 45.2 MB.

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 521.9 | 560.0 | +7.30% | faster | +7.30% | 0.36% | NO |
| 706.stockfish_r.2.4 | 545.1 | 592.1 | +8.61% | faster | +8.61% | 0.36% | NO |
| 708.sqlite_r.0.3 | 377.1 | 401.5 | +6.48% | faster | +6.48% | 1.41% | NO |
| 723.llvm_r.1.0 | 280.6 | 298.6 | +6.41% | faster | +6.41% | 1.50% | NO |
| 753.ns3_r.2.0 | 306.4 | 328.2 | +7.12% | faster | +7.12% | 1.57% | NO |

**Stats difference:** exactly one stat differs on every checkpoint,
`board.processor.start.core.executeStats0.numMiscRegReads`. `.fast` counts 8 fewer reads
on the two stockfish checkpoints and 16 fewer on the other three, out of 23–34 million.
All other stats are identical, including cycles, instructions, IPC and every predictor,
cache and pipeline counter.
- **Root cause:** `src/arch/arm/interrupts.hh:217`, `assert(checkInterrupts())` in
  `Interrupts::getInterrupt()`.
  - `checkInterrupts()` reads `HCR_EL2`, then calls `takeInt64()`
    (`src/arch/arm/interrupts.cc:117-119`), which reads `CPSR`, `SCR_EL3` and `HCR_EL2`.
  - All four reads go through `o3::CPU::readMiscReg`, which increments the stat
    (`src/cpu/o3/cpu.cc:953`). Its other helpers use the uncounted `NoEffect` path.
  - So each evaluation of the assert adds 4 to the counter in `.opt` and nothing in
    `.fast`. The observed differences, 8 and 16, are exact multiples of 4 and scale with
    the number of interrupts taken in the region.
- **Why it doesn't matter:** the reads have no architectural side effect, so the
  simulation is unchanged. `.opt`'s count is inflated by debug-only code.

**Verdict: accepted, by user ruling.** Grounds: faster on 5/5 (+6.41% to +8.61%), and the
only stats difference is the assert-inflated counter above.
- **Gate rebase (ruling 2026-09-26):** from Trial 2 on, the identity reference is the
  `trial1-fast` run, not `baseline-a`. Every later lever is therefore checked for strict
  bit-identity with no exclusions.
- **Debugging:** keep `gem5.opt`, because `--debug-flags` and asserts exist only there.

