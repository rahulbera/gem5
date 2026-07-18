# Copyright (c) 2026 Technical University of Munich
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

"""Garfield QEMU-virt FS detailed restore: QPoints snapshot -> Neoverse V2 IPC.

The QEMU_Virt sibling of ``fs_run.py``. It restores a QPoints-converted
QEMU snapshot (Track B) instead of an FS+KVM SimPoint checkpoint (Track A)
and reuses fs_run.py's validated switchable-restore machinery verbatim.
The deltas: the board is ``QemuVirtBoard`` (ArmBoard on the QEMU_Virt
platform, disk on the platform's virtio-mmio slot), and the workload is
explicit kernel/bootloader/disk paths -- no M5_PATH, no resource lookups,
no invocations.json.

Usage:
    ./build/ARM/gem5.opt --outdir=<out> configs/garfield/arm/virt_run.py \\
        --restore-dir <snapshot dir> --disk-img <prepped image> \\
        --warmup-insts 1000000 --detailed-insts 3000000
"""

import argparse
import os
import re
from pathlib import Path

import m5
from m5.objects import (
    Armv8,
    CowDiskImage,
    FetchDirectedPrefetcher,
    MemRenamePredictor,
    QEMU_Virt,
    RawDiskImage,
    VirtIOBlock,
)
from m5.util import addToPath
from m5.util.convert import toFrequency

addToPath("../..")
# isort: split

from garfield.arm import (
    cache_hierarchy,
    neoverse_v2,
)

from gem5.components.boards.arm_board import ArmBoard
from gem5.components.boards.mem_mode import MemMode
from gem5.components.memory.single_channel import (
    DIMM_DDR5_4400,
    DIMM_DDR5_6400,
    DIMM_DDR5_8400,
    SingleChannelDDR4_2400,
)
from gem5.components.processors.base_cpu_core import BaseCPUCore
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_core import SimpleCore
from gem5.components.processors.switchable_processor import (
    SwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    BootloaderResource,
    DiskImageResource,
    KernelResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.override import overrides
from gem5.utils.requires import requires

GEM5_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_BOOTLOADER = str(
    GEM5_ROOT / "system" / "arm" / "bootloader" / "arm64"
    / "boot_v2_qemu_virt.arm64"
)
DEFAULT_KERNEL = "/tmp/gem5_res/arm64-linux-kernel-6.8.12-1.0.0"

MEM_FACTORIES = {
    "DDR4_2400": SingleChannelDDR4_2400,
    "DDR5_4400": DIMM_DDR5_4400,
    "DDR5_6400": DIMM_DDR5_6400,
    "DDR5_8400": DIMM_DDR5_8400,
}


class QemuVirtBoard(ArmBoard):
    """ArmBoard on the QEMU_Virt platform. Two deltas from stock ArmBoard:
    the disk is the platform's virtio-mmio slot-31 device with the ring
    size the QEMU guest actually negotiated (1024: QEMU's legacy
    virtio-mmio reports QueueNumMax=1024 regardless of the device
    property), and no PCI device is ever attached."""

    @overrides(ArmBoard)
    def get_disk_device(self):
        return "/dev/vda"

    @overrides(ArmBoard)
    def _add_disk_to_board(self, disk_image):
        self._image = CowDiskImage(
            child=RawDiskImage(
                read_only=True, image_file=disk_image.get_local_path()
            ),
            read_only=False,
        )
        self.realview.vio[0].vio = VirtIOBlock(
            image=self._image, queueSize=1024
        )

    @overrides(ArmBoard)
    def _pre_instantiate(self, full_system=None):
        # Replicates ArmBoard._pre_instantiate WITHOUT the pci_devices
        # assignment: SimObject rejects assigning an EMPTY vector, and this
        # board attaches no PCI devices by design. Keep in sync with
        # gem5/components/boards/arm_board.py.
        root = super(ArmBoard, self)._pre_instantiate(
            full_system=full_system
        )
        self.workload.dtb_filename = self._get_dtb_filename()
        self.realview.setupBootLoader(
            self, self._get_dtb_filename(), self._bootloader
        )
        self.generateDtb(self._get_dtb_filename())
        return root


def parse_args():
    parser = argparse.ArgumentParser(
        description="Garfield QEMU-virt detailed restore (Track B)."
    )
    parser.add_argument("--restore-dir", type=str, required=True,
                        help="QPoints snapshot directory (contains m5.cpt).")
    parser.add_argument("--disk-img", type=str, required=True,
                        help="The prepped guest image the snapshot was "
                        "taken against (referenced read-only).")
    parser.add_argument("--kernel", type=str, default=DEFAULT_KERNEL,
                        help="vmlinux (same bits the guest booted).")
    parser.add_argument("--bootloader", type=str,
                        default=DEFAULT_BOOTLOADER)
    parser.add_argument("--clk-freq", type=str, default="3GHz")
    parser.add_argument("--mem-type", type=str, default="DDR5_6400",
                        choices=list(MEM_FACTORIES.keys()))
    parser.add_argument("--mem-size", type=str, default="16GiB",
                        help="MUST match the snapshot (QPoints QEMU -m).")
    parser.add_argument("--settle-insts", type=int, default=0)
    parser.add_argument("--warmup-insts", type=int, default=int(1e6))
    parser.add_argument("--detailed-insts", type=int, default=int(5e6))
    parser.add_argument("--progress-interval", type=str, default="0Hz")
    # --- prefetcher / front-end ablation (identical to fs_run.py) --------
    parser.add_argument("--disable-fdp", action="store_true")
    parser.add_argument("--disable-l1d-prefetch", action="store_true")
    parser.add_argument("--disable-l2-prefetch", action="store_true")
    # --- garfield knobs (identical to fs_run.py) -------------------------
    parser.add_argument("--ghost-exec", action="store_true")
    parser.add_argument("--use-mrn", action="store_true")
    parser.add_argument("--mrn-conf-bits", type=int, default=4)
    parser.add_argument("--mrn-conf-threshold", type=int, default=8)
    parser.add_argument("--mrn-store-entries", type=int, default=1024)
    parser.add_argument("--mrn-load-entries", type=int, default=1024)
    parser.add_argument("--mrn-mode", type=str, default="value-only",
                        choices=["value-only", "unified"])
    parser.add_argument("--mrn-correlation", type=str, default="lsq-forward",
                        choices=["lsq-forward", "store-set"])
    parser.add_argument("--mrn-allow-nonint", action="store_true")
    return parser.parse_args()


args = parse_args()

requires(isa_required=ISA.ARM)


# ---- the detailed Neoverse V2 core (copied verbatim from fs_run.py -- keep
# in sync) -------------------------------------------------------------------
def make_neoverse_v2_core():
    # cpu_id=0 must match the ATOMIC settle core: m5.switchCpus /
    # BaseCPU::takeOverFrom asserts the swapped-in and swapped-out cores
    # share a cpuId.
    core = BaseCPUCore(neoverse_v2.NeoverseV2(cpu_id=0), isa=ISA.ARM)
    cpu = core.core
    if args.disable_fdp:
        cpu.decoupledFrontEnd = False
    cpu.progress_interval = args.progress_interval
    cpu.ghostExec = args.ghost_exec
    if args.use_mrn:
        cpu.memRenamePredictor = MemRenamePredictor(
            confBits=args.mrn_conf_bits,
            confThreshold=args.mrn_conf_threshold,
            storeTableEntries=args.mrn_store_entries,
            loadTableEntries=args.mrn_load_entries,
            mrnMode=args.mrn_mode.replace("-", "_"),
            mrnCorrelation=args.mrn_correlation.replace("-", "_"),
            predictIntLoadsOnly=not args.mrn_allow_nonint,
        )
    return core


def make_atomic_core():
    AtomicClass = SimpleCore.cpu_class_factory(
        cpu_type=CPUTypes.ATOMIC, isa=ISA.ARM
    )
    return BaseCPUCore(core=AtomicClass(cpu_id=0), isa=ISA.ARM)


# ---- cache hierarchy (copied verbatim from fs_run.py -- keep in sync) ------
class RestoreNeoverseV2CacheHierarchy(
    cache_hierarchy.NeoverseV2CacheHierarchy
):
    """The identical NeoverseV2 cache hierarchy, adapted for the switchable
    restore flow: the L1I FDP prefetchers are rebound to the detailed core
    they will actually run on (ports migrate on m5.switchCpus; the FDP's
    cpu pointer and MMU do not)."""

    def __init__(self, detailed_core, **kwargs):
        super().__init__(**kwargs)
        self._detailed_cpu = detailed_core.core

    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        if not self._enable_fdp:
            return
        for l1i in self.l1icaches:
            for pf in l1i.prefetcher.prefetchers:
                if isinstance(pf, FetchDirectedPrefetcher):
                    pf.cpu = self._detailed_cpu
                    pf._mmus = [self._detailed_cpu.mmu]


# ---- restore switchable processor (copied verbatim from fs_run.py) ---------
class NeoverseV2RestoreProcessor(SwitchableProcessor):
    """start-key = Neoverse V2 (the detailed core we ultimately switch TO),
    switch-key = ATOMIC (primed active before m5.instantiate -- receives the
    checkpoint's ThreadContext)."""

    def __init__(self, detailed_core, atomic_core):
        self._start_key = "start"
        self._switch_key = "switch"
        self._current_is_start = True
        self._mem_mode = MemMode.ATOMIC
        switchable_cores = {
            self._start_key: [detailed_core],
            self._switch_key: [atomic_core],
        }
        super().__init__(
            switchable_cores=switchable_cores,
            starting_cores=self._start_key,
        )

    @overrides(SwitchableProcessor)
    def incorporate_processor(self, board):
        super().incorporate_processor(board=board)
        board.set_mem_mode(self._mem_mode)

    @overrides(SwitchableProcessor)
    def switch(self):
        if self._current_is_start:
            self.switch_to_processor(self._switch_key)
        else:
            self.switch_to_processor(self._start_key)
        self._current_is_start = not self._current_is_start

    def prime_switch_core_active(self):
        """Make the ATOMIC switch core the active one BEFORE
        m5.instantiate(), matching a checkpoint whose ThreadContext was
        serialized from a single active core."""
        self._current_cores = self._switchable_cores[self._switch_key]
        self._current_is_start = False
        for cores in self._switchable_cores.values():
            for core in cores:
                core.set_switched_out(core not in self._current_cores)


# ---- assemble the board ----------------------------------------------------
memory = MEM_FACTORIES[args.mem_type](size=args.mem_size)

detailed_core = make_neoverse_v2_core()

cache = RestoreNeoverseV2CacheHierarchy(
    detailed_core,
    enable_fdp=not args.disable_fdp,
    enable_l1d_prefetch=not args.disable_l1d_prefetch,
    enable_l2_prefetch=not args.disable_l2_prefetch,
)

processor = NeoverseV2RestoreProcessor(detailed_core, make_atomic_core())

board = QemuVirtBoard(
    clk_freq=args.clk_freq,
    processor=processor,
    memory=memory,
    cache_hierarchy=cache,
    # Match the QEMU-TCG cortex-a57 feature envelope (v8.0 + crypto):
    # a restore CPU with MORE features than the snapshot CPU executes
    # HINT-space instructions (PACIASP/AUTIASP, BTI) the guest stored as
    # NOPs -- the PAuth poison found at Gate 1. Revisit for KVM-mode
    # snapshots (Gate 3), where the envelope is the HOST's.
    release=Armv8(),
    platform=QEMU_Virt(),
)

board.set_kernel_disk_workload(
    kernel=KernelResource(os.path.abspath(args.kernel)),
    disk_image=DiskImageResource(
        os.path.abspath(args.disk_img), root_partition="2"
    ),
    bootloader=BootloaderResource(os.path.abspath(args.bootloader)),
    checkpoint=Path(args.restore_dir),
)

processor.prime_switch_core_active()

print("=== garfield QEMU-virt detailed restore (Track B) ===")
print(f"  core     : NeoverseV2 (ArmO3CPU) restore @ {args.clk_freq}")
print(f"  dram     : {args.mem_type} @ {args.mem_size}")
print(f"  mrn      : {'on (' + args.mrn_mode + ')' if args.use_mrn else 'off'}"
      f"  ghostExec={args.ghost_exec}")
print(f"  restore  : {args.restore_dir}")
print(
    f"  region   : settle={max(1, args.settle_insts):,} "
    f"warmup={args.warmup_insts:,} detailed={args.detailed_insts:,}"
)


# ---- run: restore(ATOMIC) -> switch(Neoverse V2) -> warmup -> detailed -----
def _stopper():
    while True:
        yield True


simulator = Simulator(
    board=board, on_exit_event={ExitEvent.MAX_INSTS: _stopper()}
)

settle = max(1, args.settle_insts)
print(f">>> instantiate + restore {args.restore_dir}; ATOMIC settle "
      f"{settle:,} ...")
simulator.schedule_max_insts(settle)
simulator.run()
print(f">>> restored; insts={simulator.get_instruction_count()}")

print(">>> switch ATOMIC -> Neoverse V2 (detailed) ...")
simulator.switch_processor()

if args.warmup_insts > 0:
    print(f">>> Neoverse V2 warmup {args.warmup_insts:,} insts "
          "(unmeasured) ...")
    simulator.schedule_max_insts(args.warmup_insts)
    simulator.run()

print(f">>> Neoverse V2 detailed {args.detailed_insts:,} insts (measured) ...")
m5.stats.reset()
insts0 = detailed_core.get_total_instructions()
tick0 = m5.curTick()
simulator.schedule_max_insts(args.detailed_insts)
simulator.run()
tick1 = m5.curTick()
insts1 = detailed_core.get_total_instructions()
m5.stats.dump()


# ---- best-effort summary from the just-dumped stats.txt --------------------
def _summarize():
    stats_path = Path(m5.options.outdir) / "stats.txt"
    if not stats_path.exists():
        return
    txt = stats_path.read_text()
    wanted = [
        r"processor\.start\.core.*\bipc\b",
        r"processor\.start\.core.*\bcpi\b",
        r"processor\.start\.core.*numCycles\b",
        r"processor\.start\.core.*(committedInsts|commitStats0\.numInsts)\b",
        r"^simInsts\b",
        r"^simSeconds\b",
    ]
    print("==== DETAILED-REGION STATS SUMMARY ====")
    for line in txt.splitlines():
        for pat in wanted:
            if re.search(pat, line):
                print("  " + " ".join(line.split()))
                break


_summarize()

region_insts = insts1 - insts0
region_ticks = tick1 - tick0
region_cycles = (region_ticks / 1e12) * toFrequency(args.clk_freq)
ipc = region_insts / region_cycles if region_cycles > 0 else 0.0

print(
    f"Exiting @ tick {m5.curTick()} because "
    f"{simulator.get_last_exit_event_cause()}."
)
print(f"Region insts       : {region_insts}")
print(f"Region cycles      : {region_cycles:.0f}")
print(f"IPC (measured)     : {ipc:.3f}")
print("Full stats in m5out/stats.txt")
