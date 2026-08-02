/*
 * Copyright (c) 2025 Technical University of Munich
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_SQUASH_REASON_HH__
#define __CPU_O3_SQUASH_REASON_HH__

namespace gem5
{
namespace o3
{

/**
 * Garfield: why a pipeline squash was raised / why an in-flight
 * speculative-dataflow prediction (MRN, VP) was discarded.
 *
 * A prediction consumed at rename resolves exactly once: it verifies
 * correct, verifies wrong (mispredict), or is squashed before it can
 * verify at all. This enum categorizes that third outcome; it is plumbed
 * from the squash initiator (IEW) through IEWStruct to Commit to the ROB,
 * because the cause cannot be recovered at the squash site --
 * IEW::squashInclusive is shared by real memory-order violations, both
 * MRN mispredict paths, and VP mispredicts.
 *
 * Deliberately coarse. Traps, interrupts, ReExec replays, HTM aborts,
 * ThreadContext writes, drain and squash-after all collapse into Other;
 * they are expected to be near-zero for compute-bound regions. Front-end
 * squashes (decode, FTQ, BAC) cannot appear here at all -- they discard
 * instructions upstream of rename, which have no prediction yet.
 */
enum class SquashReason
{
    /** Branch mispredict (IEW::squashDueToBranch). */
    Branch,
    /** A real memory-order violation: store->load, or load->load by snoop. */
    MemOrder,
    /** An OLDER value-forwarding MRN mispredict squashed this load. */
    MrnValue,
    /** An OLDER producer-aliasing MRN mispredict squashed this load. */
    MrnAlias,
    /** An OLDER wrong value prediction (VP) squashed this instruction. */
    ValuePred,
    /** Trap, interrupt, ReExec, HTM abort, TC write, drain, squash-after. */
    Other,
    Num
};

/** Stat subnames, indexed by SquashReason. */
constexpr const char *squashReasonNames[] = {
    "branch", "memOrder", "mrnValue", "mrnAlias", "valuePred", "other"};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_SQUASH_REASON_HH__
