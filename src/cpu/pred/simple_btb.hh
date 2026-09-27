/*
 * Copyright (c) 2022-2023 The University of Edinburgh
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#ifndef __CPU_PRED_SIMPLE_BTB_HH__
#define __CPU_PRED_SIMPLE_BTB_HH__

#include "base/cache/associative_cache.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/pred/btb.hh"
#include "cpu/pred/btb_entry.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/base.hh"
#include "params/SimpleBTB.hh"

namespace gem5::branch_prediction
{

class SimpleBTB : public BranchTargetBuffer
{
  public:
    SimpleBTB(const SimpleBTBParams &params);

    void memInvalidate() override;
    bool valid(ThreadID tid, Addr instPC) override;
    const PCStateBase *lookup(ThreadID tid, Addr instPC,
                              BranchType type = BranchType::NoBranch) override;
    void update(ThreadID tid, Addr instPC, const PCStateBase &target_pc,
                BranchType type = BranchType::NoBranch,
                StaticInstPtr inst = nullptr) override;
    const StaticInstPtr getInst(ThreadID tid, Addr instPC) override;
    bool findFirstBranch(ThreadID tid, Addr start, Addr width, Addr step,
                         Addr &addr, StaticInstPtr &inst) override;

  private:

    /** Internal call to find an address in the BTB
     * @param instPC The branch's address.
     * @return Returns a pointer to the BTB entry if found, nullptr otherwise.
    */
    BTBEntry *findEntry(Addr instPC, ThreadID tid);

    /**
     * Find an address in the BTB with the same result as btb.findEntry(),
     * without copying the set's candidate list and computing the tag once
     * rather than once per way. Like findEntry(), it updates no replacement
     * state and no stats. The fetch-target search calls it at every
     * instruction address it scans.
     */
    BTBEntry *probe(Addr instPC, ThreadID tid) const;

    /** The actual BTB. */
    AssociativeCache<BTBEntry> btb;

    /** The BTB's indexing policy if it is set associative, else nullptr. */
    const BTBSetAssociative *setAssocIndexing;

    /**
     * A packed copy of every entry's valid bit, thread and tag: one word
     * per BTB slot, in set-major order (set * assoc + way). probe() reads
     * it instead of the entries themselves, so a set's search touches
     * one cache line instead of several 100-byte entries. A slot is 0
     * while its entry is invalid. update() and memInvalidate(), the only
     * places that change entries, keep it in step. Empty (and unused) if
     * the policy is not set associative or tags are wider than 47 bits.
     */
    std::vector<uint64_t> tagMirror;

    /** Ways per set, for indexing tagMirror. */
    unsigned mirrorAssoc = 0;

    /** The tagMirror word of a valid entry with this tag and thread. */
    static uint64_t
    mirrorKey(Addr tag, ThreadID tid)
    {
        return (uint64_t(1) << 63) | (uint64_t(uint16_t(tid)) << 47) | tag;
    }
};

} // namespace gem5::branch_prediction

#endif // __CPU_PRED_SIMPLE_BTB_HH__
