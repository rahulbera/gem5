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

### Trial 2 — tcmalloc

**Summary:** linking gperftools' `tcmalloc_minimal` in place of glibc malloc speeds up
every test checkpoint by 21.2–23.0% on top of `.fast`, with stats bit-identical.
**Accepted.**

**Key idea:** gem5 allocates and frees objects at a very high rate on its hot paths. That
includes a heap-allocated `DynInst` per fetched instruction, `Request`/`Packet` pairs
per memory access, prefetcher candidates, per-lookup candidate vectors in the
set-associative tags, and branch-predictor history records. tcmalloc's per-thread free
lists serve these small allocations far more cheaply than glibc's malloc. gem5's
SConstruct already supports it: it links `tcmalloc_minimal` when the library is present
and adds `-fno-builtin-malloc/calloc/realloc/free` (`SConstruct:710`, `854-863`). No
build before this campaign had it: this box lacked `libgoogle-perftools-dev`, and the
kratos2 compute image lacks it too.

**Files and flags targeted:** `apt install libgoogle-perftools-dev` (2.15), and a new
build dir `ARM_tcm` without `--without-tcmalloc`. No source change. `ldd` confirms
`libtcmalloc_minimal.so.4`.

```
CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='' LINKFLAGS_EXTRA='' scons build/ARM_tcm/gem5.fast -j32 --ignore-style 
```

Built in 407 s.

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 560.0 | 688.9 | +23.01% | faster | +31.99% | 0.36% | yes |
| 706.stockfish_r.2.4 | 592.1 | 719.3 | +21.48% | faster | +31.95% | 0.36% | yes |
| 708.sqlite_r.0.3 | 401.5 | 492.6 | +22.70% | faster | +30.65% | 1.41% | yes |
| 723.llvm_r.1.0 | 298.6 | 362.0 | +21.24% | faster | +29.02% | 1.50% | yes |
| 753.ns3_r.2.0 | 328.2 | 402.8 | +22.74% | faster | +31.47% | 1.57% | yes |

**Verdict: accepted.** Stats are bit-identical to `trial1-fast` on 5/5, and all five are
faster, by +21.24% to +23.01%. Cumulatively the build is +29.0% to +32.0% over the
`.opt` baseline.
- **Why so large:** gem5's own "12%" claim for tcmalloc is exceeded here.
- **For the kratos2 port:** compute nodes have no tcmalloc, so the port must ship
  `libtcmalloc_minimal.so.4` next to the binary (with an rpath) or link it statically.

### Trial 3 — optimized `ext/` libraries

**Summary:** the bundled `ext/` libraries were compiled with no `-O` flag, i.e. -O0.
Building them at `-O3` gains 3.3–3.4% on the two memory-heavy checkpoints and is neutral
to slightly negative on the compute-bound ones. Stats are bit-identical.
**Accepted, narrowly.**

**Key idea:** gem5's SConstruct runs the `ext/` SConscripts on the base environment
before the per-variant `-O3` is added (`SConstruct:1003-1011` vs `src/SConscript:689-698`).
So every `ext/` library is compiled at -O0 in every variant, `.fast` included.
`ext/drampower` is on the simulation path: every DRAM command is logged to DRAMPower,
and `calcWindowEnergy` runs at each refresh (`src/mem/dram_interface.cc:1219, 1777,
1827`). `CCFLAGS_EXTRA` is appended before the `ext/` loop (`SConstruct:998-999`), so
`CCFLAGS_EXTRA=-O3` optimizes `ext/`. `src/` is unaffected because its own `-O3` comes
later.

**Files and flags targeted:** `CCFLAGS_EXTRA=-O3`, new build dir `ARM_ext`. No source
change. `compile_commands.json` confirms that all 13 `ext/drampower` sources now get `-O3`.

```
CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='-O3' LINKFLAGS_EXTRA='' scons build/ARM_ext/gem5.fast -j32 --ignore-style 
```

Built in 412 s, with 42247820 bytes of `.text`.

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 688.9 | 676.4 | -1.82% | slower | +29.59% | 0.36% | yes |
| 706.stockfish_r.2.4 | 719.3 | 712.8 | -0.91% | slower | +30.75% | 0.36% | yes |
| 708.sqlite_r.0.3 | 492.6 | 496.1 | +0.71% | faster (within noise) | +31.58% | 1.41% | yes |
| 723.llvm_r.1.0 | 362.0 | 374.2 | +3.38% | faster | +33.38% | 1.50% | yes |
| 753.ns3_r.2.0 | 402.8 | 416.3 | +3.34% | faster | +35.86% | 1.57% | yes |

**Verdict: accepted.** Stats are bit-identical on 5/5, and 3/5 are faster than Trial 2, so
the rule is met. But it is narrow:
- the gain is clear only on `723.llvm_r` (+3.38%) and `753.ns3_r` (+3.34%), the two
  lowest-IPC, most memory-active checkpoints, where DRAMPower sees the most commands;
- `708.sqlite_r`'s +0.71% is inside its noise;
- both stockfish checkpoints, compute-bound at IPC 2.3–2.5, got slower by 0.91% and
  1.82%.

The stockfish drop is larger than its baseline noise. It is more likely a code-layout
side effect than a real cost, since only `ext/` code changed. Expect this lever to help
DRAM-heavy workloads (like the agentic ones) and to be neutral elsewhere.

### Trial 4 — link-time optimization (LTO), with a linker comparison

**Summary:** building with GCC LTO shrinks `.text` from 42.2 MB to 29.8 MB. It speeds up
all five checkpoints by 1.3–6.3% (quiet-start rerun, bfd), with stats bit-identical.
The linker (bfd, gold, mold) matters about as much as run-to-run noise.
**Accepted, with bfd.**

**Key idea:** gem5 is hundreds of separately compiled files with hot calls across
them (O3 pipeline stages, caches, TLBs, branch predictor, the event queue). LTO lets
GCC inline and optimize across those file boundaries and drop unused code.
- It is opt-in via `--with-lto` (`SConstruct:125-126, 693-708`), which sets
  `-flto=<jobs>` for the compile and the link. It has been off by default since v21
  (`RELEASE-NOTES.md:1605`).
- `ext/` libraries do not get the LTO flags.

**Files and flags targeted:** `--with-lto --linker={bfd,gold,mold}` on top of the Trial 3
configuration, in a new build dir `ARM_lto`. `apt install mold` (2.30) was needed. The
first build compiles everything; the gold and mold builds only relink. Objects carry
`.gnu.lto_*` sections (1346 in `cpu/o3/cpu.fo`), confirming LTO. No source change.

```
CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='-O3' LINKFLAGS_EXTRA='' scons build/ARM_lto/gem5.fast -j32 --ignore-style --with-lto --linker=bfd
```

| Linker | Build time | Binary | `.text` |
|---|---|---|---|
| bfd | 325 s (full compile + LTO link) | 68.0 MB | 29.85 MB |
| gold | 93 s (relink only) | 68.2 MB | 29.85 MB |
| mold | 83 s (relink only) | 87.9 MB | 32.21 MB |

The first pass ran the three linkers back to back. gold and mold started with a 1-minute
load average of 3.7, which the plan requires to be re-run: the load average lags, so it
still read high right after the previous trial. `run_suite.sh` now waits for it to fall
below 1.0 before starting (up to 5 min), and records the wait. All three linkers were
re-run under that guard. Both passes are shown.

**bfd, quiet-start rerun (`trial4-lto-bfd-r2`), the configuration that was adopted:**

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 676.4 | 710.4 | +5.03% | faster | +36.11% | 0.36% | yes |
| 706.stockfish_r.2.4 | 712.8 | 757.5 | +6.27% | faster | +38.95% | 0.36% | yes |
| 708.sqlite_r.0.3 | 496.1 | 523.5 | +5.51% | faster | +38.83% | 1.41% | yes |
| 723.llvm_r.1.0 | 374.2 | 380.0 | +1.53% | faster | +35.42% | 1.50% | yes |
| 753.ns3_r.2.0 | 416.3 | 421.8 | +1.32% | faster (within noise) | +37.66% | 1.57% | yes |

**gold, quiet-start rerun:**

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 676.4 | 708.5 | +4.75% | faster | +35.75% | 0.36% | yes |
| 706.stockfish_r.2.4 | 712.8 | 738.1 | +3.56% | faster | +35.40% | 0.36% | yes |
| 708.sqlite_r.0.3 | 496.1 | 516.0 | +4.00% | faster | +36.84% | 1.41% | yes |
| 723.llvm_r.1.0 | 374.2 | 377.1 | +0.76% | faster (within noise) | +34.39% | 1.50% | yes |
| 753.ns3_r.2.0 | 416.3 | 425.8 | +2.29% | faster | +38.98% | 1.57% | yes |

**mold, quiet-start rerun:**

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 676.4 | 707.2 | +4.56% | faster | +35.51% | 0.36% | yes |
| 706.stockfish_r.2.4 | 712.8 | 735.5 | +3.19% | faster | +34.93% | 0.36% | yes |
| 708.sqlite_r.0.3 | 496.1 | 501.2 | +1.02% | faster (within noise) | +32.91% | 1.41% | yes |
| 723.llvm_r.1.0 | 374.2 | 374.4 | +0.04% | faster (within noise) | +33.43% | 1.50% | yes |
| 753.ns3_r.2.0 | 416.3 | 425.6 | +2.24% | faster | +38.91% | 1.57% | yes |

First pass, not quiet at start (for the record): bfd faster on 3/5 (mean +2.11%);
gold on 3/5 (mean +1.58%, started at load 3.74); mold on 5/5 (mean +2.52%, started at
load 3.77). Stats were bit-identical in all six runs.

| Linker | Mean change, first pass | Mean change, quiet rerun | Average |
|---|---|---|---|
| bfd | +2.11% | +3.93% | +3.02% |
| gold | +1.58% | +3.07% | +2.33% |
| mold | +2.52% | +2.21% | +2.37% |

**Verdict: accepted, with the bfd linker.** Stats are bit-identical on 5/5 for every
linker, and the quiet-start rerun is faster on 5/5 for every linker.
- **Why bfd:** it has the best quiet-start result (mean +3.93%) and the best two-pass
  average, and it is the system default, which is also simplest for the kratos2 port.
- **The linker gap is small:** about 1%, the same size as run-to-run noise. This agrees
  with gem5's own linker benchmark, which found that linkers don't matter for runtime.
- **mold is worth it for development builds:** it relinks in 83 s. It gives `.text`
  2.4 MB larger than bfd/gold.
- **Cumulative:** +35% to +39% over the `.opt` baseline.

### Trial 5a — `-march=x86-64-v2`

**Summary:** targeting x86-64-v2 (SSE4.2, POPCNT, SSSE3) on top of Trial 4 makes 4 of 5
checkpoints slower (−0.2% to −3.4%). Only one is faster, by +1.3%, which is inside its
noise. Stats are bit-identical. **Rejected.**

**Key idea:** x86-64-v2 is the newest ISA level every kratos2 node class is certain to
support. It lets GCC use `popcnt` and SSE4.x instead of the generic x86-64 baseline:
bit-count builtins become single instructions, and some loops and string/memory idioms
get better code. Since v2 has no FMA, host floating-point results cannot change.

**Files and flags targeted:** `CCFLAGS_EXTRA="-O3 -march=x86-64-v2"` and
`LINKFLAGS_EXTRA="-march=x86-64-v2"` (the link flag is needed because LTO does code
generation at link time). New build dir `ARM_v2`. No source change.
- **Flag check:** the binary has 161 `popcnt` and 1723 SSE4.x instructions; the Trial 4
  binary has none.
- **Build:** 316 s, `.text` 29.79 MB.

```
CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='-O3 -march=x86-64-v2' LINKFLAGS_EXTRA='-march=x86-64-v2' scons build/ARM_v2/gem5.fast -j32 --ignore-style --with-lto --linker=bfd
```

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 710.4 | 708.9 | -0.21% | slower (within noise) | +35.83% | 0.36% | yes |
| 706.stockfish_r.2.4 | 757.5 | 740.1 | -2.29% | slower | +35.77% | 0.36% | yes |
| 708.sqlite_r.0.3 | 523.5 | 506.0 | -3.35% | slower | +34.18% | 1.41% | yes |
| 723.llvm_r.1.0 | 380.0 | 374.3 | -1.48% | slower (within noise) | +33.42% | 1.50% | yes |
| 753.ns3_r.2.0 | 421.8 | 427.2 | +1.28% | faster (within noise) | +39.42% | 1.57% | yes |

**Verdict: rejected.** Stats are bit-identical on 5/5, but only 1/5 is faster (`753.ns3_r`
+1.28%, within noise).
- **Regressions:** `708.sqlite_r` (−3.35%) and `706.stockfish_r.2.4` (−2.29%) fell by
  more than their noise; the other two changes are within noise.
- **Why:** gem5's time goes to pointer chasing, branchy control flow and heap
  allocation, not to the bit-counting or vector idioms v2 speeds up. So v2 mostly
  perturbs code layout, and the result is neutral to slightly negative.
- **Next:** Trial 5b tries x86-64-v3 from the same base (Trial 4), not on top of v2.

### Trial 5b — `-march=x86-64-v3 -ffp-contract=off`

**Summary:** targeting x86-64-v3 (AVX, AVX2, BMI1/2, FMA, LZCNT/TZCNT, MOVBE), with
floating-point contraction disabled, **changes simulation results**. About 2000 stats
differ on every checkpoint, including simulated time and cache hit/miss counts. It is
also slower on 4 of 5 checkpoints. **Rejected**, on both grounds.

**Key idea:** v3 adds 256-bit vectors and BMI2 bit manipulation. `-ffp-contract=off` was
added so that GCC could not fuse multiply-adds, since FMA changes host floating-point
rounding. The goal was faster simulation with identical results.

**Deployability check:** one tiny `srun --immediate` probe per kratos2 node class:
- **kratos0–9:** kratos9, Xeon Gold 5118 (Skylake-SP): AVX2, AVX-512.
- **kratos11–19:** kratos12 and kratos17, Xeon Gold 6226R (Cascade Lake): AVX2, AVX-512.
- **safari-nexus1:** AMD EPYC 9554 (Zen 4): AVX2, AVX-512.
- **kratos10:** the single 256-CPU node was busy and is not verified.

So v3 would have been deployable on every verified class.

**Files and flags targeted:** `CCFLAGS_EXTRA="-O3 -march=x86-64-v3 -ffp-contract=off"` and
the same `-march`/`-ffp-contract` in `LINKFLAGS_EXTRA` (LTO), on top of Trial 4 (not on
top of 5a). New build dir `ARM_v3`. No source change.
- **Flag check:** 38,434 `ymm` and 9,514 BMI2 instructions, against zero in Trial 4.
  Only 3 FMA instructions remain; they come from explicit `fma()` calls, which are
  exactly rounded either way.
- **Build:** 317 s, `.text` 30.58 MB.

```
CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='-O3 -march=x86-64-v3 -ffp-contract=off' LINKFLAGS_EXTRA='-march=x86-64-v3 -ffp-contract=off' scons build/ARM_v3/gem5.fast -j32 --ignore-style --with-lto --linker=bfd
```

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 710.4 | 706.4 | -0.56% | slower | +35.35% | 0.36% | NO |
| 706.stockfish_r.2.4 | 757.5 | 735.2 | -2.95% | slower | +34.86% | 0.36% | NO |
| 708.sqlite_r.0.3 | 523.5 | 505.8 | -3.38% | slower | +34.14% | 1.41% | NO |
| 723.llvm_r.1.0 | 380.0 | 385.0 | +1.34% | faster (within noise) | +37.23% | 1.50% | NO |
| 753.ns3_r.2.0 | 421.8 | 412.0 | -2.32% | slower | +34.46% | 1.57% | NO |

Differing stats per checkpoint against `trial1-fast`: 706.stockfish_r.1.1: 2183, 706.stockfish_r.2.4: 1964, 708.sqlite_r.0.3: 2106, 723.llvm_r.1.0: 2407, 753.ns3_r.2.0: 1919.

**Verdict: rejected.**
1. **Results change.** Every checkpoint diverges: simulated ticks, L1D hits and misses,
   DRAM traffic, and the instruction count at the stop point (a few instructions either
   way).
2. **It isn't faster:** 4/5 slower (−0.56% to −3.38%), one +1.34% within noise.

**Finding for part 2 (correctness, not speed):** the fork's simulation results depend on
the host code-generation target. `x86-64-v2` was bit-identical; `x86-64-v3` is not.
- FMA contraction is ruled out: it is disabled, and the 3 remaining FMAs are exactly
  rounded.
- `ctz`/`clz` of zero is ruled out: gem5's bit helpers guard zero
  (`src/base/bitfield.hh`), and the only raw builtins are in SVE code, which is unused.
- **Leading suspect:** a read of uninitialized or out-of-bounds memory whose contents
  depend on generated code. One candidate is already known: the TAGE `BranchInfo`
  arrays allocated with `new int[...]` and no initialization (`src/cpu/pred/tage_base.hh`,
  and `tableIndices` in `tage_sc_l.cc`).
- **Until this is fixed:** never mix gem5 binaries built with different `-march` in
  one experiment. The kratos2 build keeps the generic x86-64 target.

### Trial 6 — profile-guided optimization (PGO)

**Summary:** GCC PGO, trained on three SPEC checkpoints that are **not** in the test
suite, speeds up all five test checkpoints by 8.1–11.3% on top of Trial 4. Stats are
bit-identical. **Accepted by the gate.** Whether it belongs in the everyday build is a
workflow decision, discussed below.

**Key idea:** gem5 is front-end bound with a flat profile: many functions, heavy virtual
dispatch, and branches whose bias is known only at run time. PGO gives GCC real
branch probabilities, call counts and hot/cold information, which drive inlining, basic-
block layout, hot/cold function splitting and loop decisions. Code that never ran in
training keeps normal optimization, thanks to `-fprofile-partial-training`, so
workloads outside the training set are not penalized.

**Files and flags targeted:** no source change. Everything is in build dir `ARM_pgo`, on
top of the accepted Trial 4 configuration (`.fast`, tcmalloc, `ext/` at `-O3`,
`--with-lto --linker=bfd`).
1. **Instrumented build** (412 s; 750 MB binary, 74.2 MB `.text`):

   ```
   CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='-O3 -fprofile-generate' LINKFLAGS_EXTRA='-fprofile-generate' scons build/ARM_pgo/gem5.fast -j32 --ignore-style --with-lto --linker=bfd
   ```

2. **Training:** `run_suite.sh build/ARM_pgo/gem5.fast pgo-train train` on the three
   training checkpoints (10M+30M each, concurrent). It wrote 2034 `.gcda` files
   (74.3 MB). Instrumented speed: 721.gcc_r.2.0 177.7 KIPS, wall 294.4s; 748.flightdm_r.2.2 229.0 KIPS, wall 212.7s; 767.nest_r.1.0 403.5 KIPS, wall 134.9s; 
3. **Optimized build** in the same dir (266 s). No profile mismatch warnings; the
   binary has no gcov instrumentation; 73.2 MB binary, 34.84 MB `.text`. The code
   grows 17% over Trial 4's 29.85 MB because of profile-driven inlining and unrolling
   on hot paths.

   ```
   CC=gcc-12 CXX=g++-12 CCFLAGS_EXTRA='-O3 -fprofile-use -fprofile-partial-training -Wno-missing-profile' LINKFLAGS_EXTRA='-fprofile-use -fprofile-partial-training' scons build/ARM_pgo/gem5.fast -j32 --ignore-style --with-lto --linker=bfd
   ```

**Test suite (disjoint from training):**

| Checkpoint | Previous KIPS | New KIPS | Change | Direction | Cumulative vs baseline | Baseline noise | Stats identical |
|---|---|---|---|---|---|---|---|
| 706.stockfish_r.1.1 | 710.4 | 789.6 | +11.15% | faster | +51.29% | 0.36% | yes |
| 706.stockfish_r.2.4 | 757.5 | 822.9 | +8.64% | faster | +50.96% | 0.36% | yes |
| 708.sqlite_r.0.3 | 523.5 | 566.0 | +8.13% | faster | +50.12% | 1.41% | yes |
| 723.llvm_r.1.0 | 380.0 | 419.5 | +10.42% | faster | +49.52% | 1.50% | yes |
| 753.ns3_r.2.0 | 421.8 | 469.3 | +11.27% | faster | +53.17% | 1.57% | yes |

**Verdict: accepted by the gate rule.** Stats are bit-identical on 5/5, and all five are
faster, by +8.13% to +11.27%.
- **Cumulative:** +49.5% to +53.2% over the `.opt` baseline.
- **It generalizes:** the gain appears on workloads the profile never saw (sqlite,
  stockfish, llvm, ns3; trained on flightdm, gcc, nest).

**Workflow cost:** a PGO build costs instrumented build + training + optimized build,
about 412 s + ~5 min + 266 s ≈ 16 min, against ~5.4 min for a plain LTO build.
- **Staleness is gradual.** When source files change, GCC drops the profile only for the
  functions whose code changed (they fall back to normal optimization under
  `-fprofile-partial-training`). The rest of the simulator keeps its profile, so
  retraining is occasional, not per build.
- **Open decision:** raised by the user during this trial. Is the ~10% worth this
  pipeline in an actively developed simulator? One option is PGO only for
  bulk-experiment binaries, with plain LTO (or mold for fast relinks) for development.

