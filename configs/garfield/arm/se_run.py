# Copyright (c) 2025 Technical University of Munich
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Garfield ARM SE simulation driver.

Assembles a Neoverse V2-class out-of-order ARM core with its full cache
hierarchy (L1I/L1D 64KiB, L2 2MiB; prefetchers + FDP on by default) and a
single-channel DDR5 memory, then runs a syscall-emulation (SE) workload.

Usage:
    ./build/ARM/gem5.opt configs/garfield/arm/se_run.py
    ./build/ARM/gem5.opt configs/garfield/arm/se_run.py \\
        --binary /path/to/aarch64.elf --args "a b c"
"""

import argparse
import os
import shlex

import m5
from m5.util import addToPath
from m5.util.convert import toFrequency

addToPath("../..")
# isort: split

from garfield.arm import (
    cache_hierarchy,
    neoverse_v2,
)

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory.single_channel import (
    DIMM_DDR5_4400,
    DIMM_DDR5_6400,
    DIMM_DDR5_8400,
    SingleChannelDDR4_2400,
)
from gem5.components.processors.base_cpu_core import BaseCPUCore
from gem5.components.processors.base_cpu_processor import (
    BaseCPUProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    BinaryResource,
    obtain_resource,
)
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

MEM_FACTORIES = {
    "DDR4_2400": SingleChannelDDR4_2400,
    "DDR5_4400": DIMM_DDR5_4400,
    "DDR5_6400": DIMM_DDR5_6400,
    "DDR5_8400": DIMM_DDR5_8400,
}

SMOKE_TESTS = {"hello": "arm-hello64-static"}


def parse_args():
    parser = argparse.ArgumentParser(
        description="Garfield ARM Neoverse V2 SE simulation."
    )
    parser.add_argument(
        "--binary",
        type=str,
        default=None,
        help="Local AArch64 ELF to run. If omitted, runs the built-in "
        "'hello' smoke test.",
    )
    parser.add_argument(
        "--args",
        type=str,
        default="",
        help="Workload arguments (shell-split).",
    )
    parser.add_argument(
        "--workload",
        type=str,
        default="hello",
        choices=list(SMOKE_TESTS.keys()),
        help="Built-in smoke-test workload (used only when --binary is "
        "omitted).",
    )
    parser.add_argument(
        "--num-cores",
        type=int,
        default=1,
        help="Number of SE cores; the same binary runs on each.",
    )
    parser.add_argument(
        "--clk-freq",
        type=str,
        default="3GHz",
        help="Core/board clock frequency.",
    )
    parser.add_argument(
        "--mem-type",
        type=str,
        default="DDR5_6400",
        choices=list(MEM_FACTORIES.keys()),
        help="DRAM model preset.",
    )
    parser.add_argument(
        "--mem-size",
        type=str,
        default="4GiB",
        help="DRAM capacity.",
    )
    parser.add_argument(
        "--max-insts",
        type=int,
        default=0,
        help="Cap committed instructions (0 = unlimited).",
    )
    parser.add_argument(
        "--disable-fdp",
        action="store_true",
        help="Disable the decoupled front-end + L1I FDP prefetcher.",
    )
    parser.add_argument(
        "--disable-l1d-prefetch",
        action="store_true",
        help="Disable the L1D Stride+SMS prefetcher stack.",
    )
    parser.add_argument(
        "--disable-l2-prefetch",
        action="store_true",
        help="Disable the L2 BOP prefetcher.",
    )
    parser.add_argument(
        "--progress-interval",
        type=str,
        default="0Hz",
        help="CPU progress-heartbeat frequency (e.g. 1kHz); 0Hz disables.",
    )
    parser.add_argument(
        "--ghost-exec",
        action="store_true",
        help="Garfield: ghost-execute control uops (skip OoO IQ entry, "
        "issue bandwidth, execution port).",
    )
    return parser.parse_args()


args = parse_args()

requires(isa_required=ISA.ARM)

memory = MEM_FACTORIES[args.mem_type](size=args.mem_size)

cache = cache_hierarchy.NeoverseV2CacheHierarchy(
    enable_fdp=not args.disable_fdp,
    enable_l1d_prefetch=not args.disable_l1d_prefetch,
    enable_l2_prefetch=not args.disable_l2_prefetch,
)

cores = [
    BaseCPUCore(neoverse_v2.NeoverseV2(), isa=ISA.ARM)
    for _ in range(args.num_cores)
]
processor = BaseCPUProcessor(cores=cores)
for core in processor.get_cores():
    cpu = core.core
    if args.disable_fdp:
        cpu.decoupledFrontEnd = False
    if args.max_insts > 0:
        cpu.max_insts_any_thread = args.max_insts
    cpu.progress_interval = args.progress_interval
    cpu.ghostExec = args.ghost_exec

board = SimpleBoard(
    clk_freq=args.clk_freq,
    processor=processor,
    memory=memory,
    cache_hierarchy=cache,
)

if args.binary:
    binary = BinaryResource(local_path=os.path.abspath(args.binary))
    workload_name = args.binary
else:
    resource_id = SMOKE_TESTS[args.workload]
    binary = obtain_resource(resource_id)
    workload_name = f"{resource_id} ({args.workload} smoke test)"
board.set_se_binary_workload(
    binary, arguments=shlex.split(args.args) if args.args else []
)

print("=== garfield ARM SE machine ===")
print(
    f"  core     : NeoverseV2 (ArmO3CPU) x{args.num_cores} "
    f"@ {args.clk_freq}"
)
print(
    f"  caches   : L1I/L1D 64KiB, L2 2MiB | "
    f"FDP={'off' if args.disable_fdp else 'on'}, "
    f"L1Dpf={'off' if args.disable_l1d_prefetch else 'on'}, "
    f"L2pf={'off' if args.disable_l2_prefetch else 'on'}"
)
print(f"  dram     : {args.mem_type} @ {args.mem_size}")
print(f"  workload : {workload_name}")

simulator = Simulator(board=board)
simulator.run()

print(
    f"Exiting @ tick {m5.curTick()} because "
    f"{simulator.get_last_exit_event_cause()}."
)
sim_seconds = m5.curTick() / 1e12
total_insts = sum(
    core.get_total_instructions() for core in processor.get_cores()
)
cycles = sim_seconds * toFrequency(args.clk_freq)
ipc = total_insts / cycles if cycles > 0 else 0.0
print(f"Simulated seconds  : {sim_seconds:.9f}")
print(f"Committed insts    : {total_insts}")
print(f"IPC (approx)       : {ipc:.3f}")
print("Full stats in m5out/stats.txt")
