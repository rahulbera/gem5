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

"""Simulation knobs shared by every garfield ARM run-mode driver.

``se_run.py`` (syscall emulation) and ``fs_run.py`` (full-system checkpoint
restore) build the *same* Neoverse V2 machine and differ only in how the
workload gets in front of it. Everything that describes the machine rather
than the run mode lives here so the drivers cannot drift apart:

  * the DRAM model presets (``MEM_FACTORIES``) and ``--clk-freq`` /
    ``--mem-type`` / ``--mem-size``,
  * the front-end / prefetcher ablation switches (``--disable-fdp``,
    ``--disable-l1d-prefetch``, ``--disable-l2-prefetch``),
  * ``--progress-interval``,
  * the garfield knobs (``--ghost-exec`` and the ``--use-mrn`` / ``--mrn-*``
    memory-rename-predictor family).

A driver registers them with :func:`add_common_args`, then turns the parsed
namespace into objects with :func:`make_memory`, :func:`cache_kwargs` and
:func:`apply_core_knobs`. Run-mode-specific knobs (the workload, the
checkpoint, the region lengths) stay in the driver that owns them.
"""

from m5.objects import MemRenamePredictor

from gem5.components.memory.single_channel import (
    DIMM_DDR5_4400,
    DIMM_DDR5_6400,
    DIMM_DDR5_8400,
    SingleChannelDDR4_2400,
)

MEM_FACTORIES = {
    "DDR4_2400": SingleChannelDDR4_2400,
    "DDR5_4400": DIMM_DDR5_4400,
    "DDR5_6400": DIMM_DDR5_6400,
    "DDR5_8400": DIMM_DDR5_8400,
}


def add_common_args(
    parser,
    mem_size_default="16GiB",
    mem_size_help="Guest RAM capacity.",
):
    """Register the shared simulation knobs on ``parser``.

    ``mem_size_default`` / ``mem_size_help`` are parameterized because the
    run modes disagree: SE picks its own DRAM size freely, while an FS
    restore MUST match the size baked into the checkpoint.
    """
    machine = parser.add_argument_group("machine")
    machine.add_argument(
        "--clk-freq",
        type=str,
        default="3GHz",
        help="Core/board clock frequency.",
    )
    machine.add_argument(
        "--mem-type",
        type=str,
        default="DDR5_6400",
        choices=list(MEM_FACTORIES.keys()),
        help="DRAM model preset.",
    )
    machine.add_argument(
        "--mem-size",
        type=str,
        default=mem_size_default,
        help=mem_size_help,
    )
    machine.add_argument(
        "--progress-interval",
        type=str,
        default="0Hz",
        help="CPU progress-heartbeat frequency (e.g. 1kHz); 0Hz disables.",
    )

    prefetch = parser.add_argument_group("prefetcher / front-end ablation")
    prefetch.add_argument(
        "--disable-fdp",
        action="store_true",
        help="Disable the decoupled front-end + L1I FDP prefetcher.",
    )
    prefetch.add_argument(
        "--disable-l1d-prefetch",
        action="store_true",
        help="Disable the L1D Stride+SMS prefetcher stack.",
    )
    prefetch.add_argument(
        "--disable-l2-prefetch",
        action="store_true",
        help="Disable the L2 BOP prefetcher.",
    )

    garfield = parser.add_argument_group("garfield")
    garfield.add_argument(
        "--ghost-exec",
        action="store_true",
        help="Garfield: ghost-execute control uops (skip OoO IQ entry, "
        "issue bandwidth, execution port).",
    )
    garfield.add_argument(
        "--use-mrn",
        action="store_true",
        help="Garfield: attach the memory-rename predictor (MRN). When "
        "absent, MRN stays disabled (NULL).",
    )
    garfield.add_argument(
        "--mrn-conf-bits",
        type=int,
        default=4,
        help="MRN confidence-counter width in bits.",
    )
    garfield.add_argument(
        "--mrn-conf-threshold",
        type=int,
        default=8,
        help="MRN minimum confidence required to predict.",
    )
    garfield.add_argument(
        "--mrn-store-entries",
        type=int,
        default=1024,
        help="MRN store-cache entries.",
    )
    garfield.add_argument(
        "--mrn-load-entries",
        type=int,
        default=1024,
        help="MRN load-cache entries.",
    )
    garfield.add_argument(
        "--mrn-mode",
        type=str,
        default="value-only",
        choices=["value-only", "unified"],
        help="MRN mode: value-only (mode B value snapshot only) or unified "
        "(mode C, which subsumes B: alias to an in-flight producer's physreg "
        "when found, else fall back to the B value snapshot).",
    )
    garfield.add_argument(
        "--mrn-correlation",
        type=str,
        default="lsq-forward",
        choices=["lsq-forward", "store-set"],
        help="Producer-binding source for unified mode: lsq-forward "
        "(loadPC->storePC learned from LSQ forwarding) or store-set (stub: "
        "no producer found).",
    )
    garfield.add_argument(
        "--mrn-allow-nonint",
        action="store_true",
        help="Relax MRN's integer-only forwarding restriction. UNSAFE: "
        "mode B forwards a scalar value, so forwarding a non-integer "
        "(vector/FP) load will panic. Default off (integer loads only).",
    )
    return parser


def make_memory(args):
    """The single-channel DRAM selected by --mem-type / --mem-size."""
    return MEM_FACTORIES[args.mem_type](size=args.mem_size)


def cache_kwargs(args):
    """The NeoverseV2CacheHierarchy ablation kwargs implied by ``args``.

    Returned as a dict rather than a built hierarchy so a driver can feed
    them to a subclass (the FS restore wraps the hierarchy to rebind FDP).
    """
    return {
        "enable_fdp": not args.disable_fdp,
        "enable_l1d_prefetch": not args.disable_l1d_prefetch,
        "enable_l2_prefetch": not args.disable_l2_prefetch,
    }


def make_mrn(args):
    """The MemRenamePredictor described by the --mrn-* knobs, or None."""
    if not args.use_mrn:
        return None
    return MemRenamePredictor(
        confBits=args.mrn_conf_bits,
        confThreshold=args.mrn_conf_threshold,
        storeTableEntries=args.mrn_store_entries,
        loadTableEntries=args.mrn_load_entries,
        mrnMode=args.mrn_mode.replace("-", "_"),
        mrnCorrelation=args.mrn_correlation.replace("-", "_"),
        predictIntLoadsOnly=not args.mrn_allow_nonint,
    )


def apply_core_knobs(cpu, args):
    """Apply the shared per-core knobs to one ArmO3CPU (Neoverse V2)."""
    if args.disable_fdp:
        cpu.decoupledFrontEnd = False
    cpu.progress_interval = args.progress_interval
    cpu.ghostExec = args.ghost_exec
    mrn = make_mrn(args)
    if mrn is not None:
        cpu.memRenamePredictor = mrn


def describe(args):
    """The machine banner lines implied by the shared knobs."""
    return [
        f"  caches   : L1I/L1D 64KiB, L2 2MiB | "
        f"FDP={'off' if args.disable_fdp else 'on'}, "
        f"L1Dpf={'off' if args.disable_l1d_prefetch else 'on'}, "
        f"L2pf={'off' if args.disable_l2_prefetch else 'on'}",
        f"  dram     : {args.mem_type} @ {args.mem_size}",
        f"  mrn      : "
        f"{'on (' + args.mrn_mode + ')' if args.use_mrn else 'off'}"
        f"  ghostExec={args.ghost_exec}",
    ]
