/*
 * Copyright (c) 2026 The Regents of the University of California
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

#ifndef __CPU_O3_RUN_AHEAD_ENGINE_HH__
#define __CPU_O3_RUN_AHEAD_ENGINE_HH__

#include <queue>
#include <vector>

#include "arch/generic/pcstate.hh"
#include "cpu/base.hh"
#include "cpu/exec_context.hh"
#include "cpu/o3/oracle_info.hh"
#include "cpu/o3/run_ahead_fifo.hh"
#include "cpu/o3/run_ahead_overlay.hh"
#include "cpu/reg_class.hh"
#include "cpu/simple_thread.hh"
#include "cpu/static_inst.hh"
#include "mem/request.hh"
#include "params/RunAheadEngine.hh"

namespace gem5
{

class ThreadContext;
class Process;

namespace o3
{

/**
 * Execute-at-fetch run-ahead engine (v1 substrate).
 *
 * Structurally a stripped-down CheckerCPU: a BaseCPU side-car that is also an
 * ExecContext, owning a shadow SimpleThread, MMU, decoder, and memory ports.
 * Instead of *verifying* committed instructions like the checker, it runs the
 * true instruction path *ahead* of the O3 timing pipeline and records each
 * (micro-)instruction's ground truth (OracleInfo) into a FIFO. The pipeline
 * attaches that truth to the matching DynInst at fetch and, at commit, asserts
 * it equals the real execution. v1 is metadata only -- the pipeline's
 * behaviour is unchanged.
 *
 * Like the checker it is built with BaseCPU(params, is_checker=true) so it
 * does not register as a schedulable CPU and does not perturb the simulation.
 * Its data memory goes through a private copy-on-write overlay so its
 * ahead-of-commit stores never disturb the timing model's memory.
 */
class DynInst;

class RunAheadEngine : public BaseCPU, public ExecContext
{
  protected:
    /** Id attached to all issued (functional) requests. */
    RequestorID requestorId;

  public:
    PARAMS(RunAheadEngine);
    RunAheadEngine(const Params &p);
    ~RunAheadEngine();

    void init() override;

    /** Build the shadow SimpleThread (called by the owning O3 CPU). */
    void setSystem(System *system);
    void setIcachePort(RequestPort *icache_port);
    void setDcachePort(RequestPort *dcache_port);

    Port &
    getDataPort() override
    {
        assert(dcachePort);
        return *dcachePort;
    }

    Port &
    getInstPort() override
    {
        assert(icachePort);
        return *icachePort;
    }

    bool
    enabled() const
    {
        return enable;
    }
    bool
    validating() const
    {
        return validateOracle;
    }

    // --- Run-ahead substrate interface (used by the O3 pipeline) ---

    /**
     * (Re)initialize the shadow architectural state from @p src_tc and reset
     * the run-ahead frontier. Called when the engine is activated: at program
     * start, after a checkpoint restore, after a CPU switch-in, and after a
     * syscall commits.
     */
    void initFromThreadContext(gem5::ThreadContext *src_tc);

    /**
     * Ensure OracleInfo has been produced for the instruction at fetch PC
     * (pc, upc) and, if it is the next true instruction, return it. Returns
     * nullptr when the fetch is off the true path (wrong-path) or blocked at a
     * syscall barrier. (Implemented with fetch alignment in a later change.)
     */
    const OracleInfo *consumeAtFetch(Addr pc, MicroPC upc);

    /** Whether run-ahead is paused at a syscall barrier. */
    bool
    atSyscallBarrier() const
    {
        return syscallBarrier;
    }

    /**
     * At commit, assert the attached OracleInfo equals @p inst's real
     * execution (destination register values + next-PC + fault), then release
     * the FIFO entry. Skips isUnverifiable and syscall instructions.
     */
    void validateAtCommit(DynInst *inst);

    /**
     * Realign the run-ahead frontier after a squash: rewind the FIFO consume
     * pointer to @p inst's record (if @p reFetchInst, e.g. a memory-order
     * violation re-fetches the inst itself) or to the record after it (a
     * branch misprediction / squash-after, where @p inst survives and only
     * younger instructions are re-fetched).
     */
    void onSquash(DynInst *inst, bool reFetchInst);

    /**
     * After a syscall commits, resync the shadow state from the post-syscall
     * thread context (the timing model is the syscall authority) and resume
     * run-ahead production from the barrier.
     */
    void onSyscallCommit(gem5::ThreadContext *post_tc);

  protected:
    std::vector<Process *> workload;
    System *systemPtr;

    RequestPort *icachePort;
    RequestPort *dcachePort;

    gem5::ThreadContext *tc;
    BaseMMU *mmu;

    StaticInstPtr curStaticInst;
    StaticInstPtr curMacroStaticInst;

    /** Number of true (micro-)instructions produced. */
    Counter numInst;

  public:
    /** Shadow thread holding the run-ahead architectural state. */
    SimpleThread *thread;

    BaseMMU *
    getMMUPtr()
    {
        return mmu;
    }

    Counter
    totalInsts() const override
    {
        return numInst;
    }
    Counter
    totalOps() const override
    {
        return numInst;
    }

    void
    wakeup(ThreadID tid) override
    {}

    // ---- ExecContext register interface (mirrors CheckerCPU) ----
    // The accessor methods take the instruction's *operand* index, not the
    // architectural register index, so renaming is transparent. A raw
    // StaticInst pointer is used to avoid ref-count overhead.

    RegVal
    getRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &id = si->srcRegIdx(idx);
        if (id.is(InvalidRegClass)) {
            return 0;
        }
        return thread->getReg(id);
    }

    void
    getRegOperand(const StaticInst *si, int idx, void *val) override
    {
        thread->getReg(si->srcRegIdx(idx), val);
    }

    void *
    getWritableRegOperand(const StaticInst *si, int idx) override
    {
        return thread->getWritableReg(si->destRegIdx(idx));
    }

    void
    setRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const RegId &id = si->destRegIdx(idx);
        if (id.is(InvalidRegClass)) {
            return;
        }
        const RegId flat = id.flatten(*thread->getIsaPtr());
        thread->setReg(flat, val);
        recordDestScalar(flat, val);
    }

    void
    setRegOperand(const StaticInst *si, int idx, const void *val) override
    {
        const RegId &id = si->destRegIdx(idx);
        if (id.is(InvalidRegClass)) {
            return;
        }
        const RegId flat = id.flatten(*thread->getIsaPtr());
        thread->setReg(flat, val);
        recordDestWide(flat);
    }

    bool
    readPredicate() const override
    {
        return thread->readPredicate();
    }
    void
    setPredicate(bool val) override
    {
        thread->setPredicate(val);
    }

    bool
    readMemAccPredicate() const override
    {
        return thread->readMemAccPredicate();
    }
    void
    setMemAccPredicate(bool val) override
    {
        thread->setMemAccPredicate(val);
    }

    uint64_t
    getHtmTransactionUid() const override
    {
        return 0;
    }
    uint64_t
    newHtmTransactionUid() const override
    {
        return 0;
    }
    bool
    inHtmTransactionalState() const override
    {
        return false;
    }
    uint64_t
    getHtmTransactionalDepth() const override
    {
        return 0;
    }

    Fault
    initiateMemMgmtCmd(Request::Flags flags) override
    {
        panic("RunAheadEngine: memory-management commands not supported");
    }

    const PCStateBase &
    pcState() const override
    {
        return thread->pcState();
    }
    void
    pcState(const PCStateBase &val) override
    {
        thread->pcState(val);
    }

    RegVal
    readMiscRegNoEffect(int misc_reg) const
    {
        return thread->readMiscRegNoEffect(misc_reg);
    }
    RegVal
    readMiscReg(int misc_reg) override
    {
        return thread->readMiscReg(misc_reg);
    }
    void
    setMiscReg(int misc_reg, RegVal val) override
    {
        thread->setMiscReg(misc_reg, val);
    }

    RegVal
    readMiscRegOperand(const StaticInst *si, int idx) override
    {
        const RegId &reg = si->srcRegIdx(idx);
        assert(reg.is(MiscRegClass));
        return thread->readMiscReg(reg.index());
    }
    void
    setMiscRegOperand(const StaticInst *si, int idx, RegVal val) override
    {
        const RegId &reg = si->destRegIdx(idx);
        assert(reg.is(MiscRegClass));
        thread->setMiscReg(reg.index(), val);
    }

    void
    demapPage(Addr vaddr, uint64_t asn) override
    {
        mmu->demapPage(vaddr, asn);
    }

    // monitor/mwait are not exercised by SE compute workloads.
    void
    armMonitor(Addr address) override
    {
        BaseCPU::armMonitor(0, address);
    }
    bool
    mwait(PacketPtr pkt) override
    {
        return BaseCPU::mwait(0, pkt);
    }
    void
    mwaitAtomic(gem5::ThreadContext *atc) override
    {
        BaseCPU::mwaitAtomic(0, atc, thread->mmu);
    }
    AddressMonitor *
    getAddrMonitor() override
    {
        return BaseCPU::getCpuAddrMonitor(0);
    }

    RequestPtr genMemFragmentRequest(Addr frag_addr, int size,
                                     Request::Flags flags,
                                     const std::vector<bool> &byte_enable,
                                     int &frag_size, int &size_left) const;

    Fault readMem(Addr addr, uint8_t *data, unsigned size,
                  Request::Flags flags,
                  const std::vector<bool> &byte_enable) override;

    Fault writeMem(uint8_t *data, unsigned size, Addr addr,
                   Request::Flags flags, uint64_t *res,
                   const std::vector<bool> &byte_enable) override;

    Fault amoMem(Addr addr, uint8_t *data, unsigned size, Request::Flags flags,
                 AtomicOpFunctorPtr amo_op) override;

    unsigned int
    readStCondFailures() const override
    {
        return thread->readStCondFailures();
    }
    void
    setStCondFailures(unsigned int sc_failures) override
    {}

    gem5::ThreadContext *
    tcBase() const override
    {
        return tc;
    }
    SimpleThread *
    threadBase()
    {
        return thread;
    }

  private:
    const bool enable;
    const bool validateOracle;
    const int injectOracleFault;

    /** Private copy-on-write view of the engine's ahead-of-commit stores. */
    RunAheadStoreOverlay overlay;
    /** In-order buffer of produced OracleInfo, keyed by true index. */
    OracleFifo fifo;

    /** The record currently being filled by produceNext()/setRegOperand(). */
    OracleInfo *curOracle = nullptr;
    /** Whether produced-but-not-yet-consumed run-ahead has stopped. */
    bool syscallBarrier = false;
    bool initialized = false;

    /** Functionally execute the next true (micro-)instruction, recording its
     *  OracleInfo into the FIFO. Returns false if it stops at a syscall. */
    bool produceNext();

    /** Read instruction bytes for the shadow PC (functional). */
    Fault fetchInstMem();

    void recordDestScalar(const RegId &flat, RegVal val);
    void recordDestWide(const RegId &flat);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RUN_AHEAD_ENGINE_HH__
