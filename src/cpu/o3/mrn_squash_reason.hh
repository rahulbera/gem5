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

#ifndef __CPU_O3_MRN_SQUASH_REASON_HH__
#define __CPU_O3_MRN_SQUASH_REASON_HH__

namespace gem5
{
namespace o3
{

/**
 * Garfield MRN: why an in-flight memory-renamed load was discarded.
 *
 * A load that is MRN-forwarded at rename resolves exactly once: it either
 * verifies correct, verifies wrong (mispredict), or is squashed before it
 * can verify at all. This enum categorizes that third outcome.
 *
 * The reason cannot be recovered at the squash site: IEW::squashDueToMemOrder
 * is shared by real memory-order violations AND both MRN mispredict paths, so
 * it is plumbed from the initiator through IEWStruct to Commit to the ROB.
 *
 * Deliberately coarse. Traps, interrupts, ReExec replays, HTM aborts,
 * ThreadContext writes, drain and squash-after all collapse into Other; they
 * are expected to be near-zero for compute-bound regions. Front-end squashes
 * (decode, FTQ, BAC) cannot appear here at all -- they discard instructions
 * upstream of rename, which have no MRN prediction yet.
 */
enum class MrnSquashReason
{
    /** Branch mispredict (IEW::squashDueToBranch). */
    Branch,
    /** A real memory-order violation: store->load, or load->load by snoop. */
    MemOrder,
    /** An OLDER value-forwarding MRN mispredict squashed this load. */
    MrnValue,
    /** An OLDER producer-aliasing MRN mispredict squashed this load. */
    MrnAlias,
    /** Trap, interrupt, ReExec, HTM abort, TC write, drain, squash-after. */
    Other,
    Num
};

/** Stat subnames, indexed by MrnSquashReason. */
constexpr const char *mrnSquashReasonNames[] = {
    "branch", "memOrder", "mrnValue", "mrnAlias", "other"};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MRN_SQUASH_REASON_HH__
