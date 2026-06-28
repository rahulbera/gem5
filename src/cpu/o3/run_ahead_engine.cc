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

#include "cpu/o3/run_ahead_engine.hh"

#include <cstring>
#include <memory>

#include "arch/generic/decoder.hh"
#include "arch/generic/isa.hh"
#include "arch/generic/mmu.hh"
#include "base/amo.hh"
#include "base/cprintf.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/inst_res.hh"
#include "cpu/null_static_inst.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Oracle.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/se_translating_port_proxy.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

RunAheadEngine::RunAheadEngine(const Params &p)
    : BaseCPU(p, true),
      requestorId(0),
      systemPtr(nullptr),
      icachePort(nullptr),
      dcachePort(nullptr),
      tc(nullptr),
      mmu(p.mmu),
      curStaticInst(nullStaticInstPtr),
      curMacroStaticInst(nullStaticInstPtr),
      numInst(0),
      thread(nullptr),
      enable(p.enable),
      validateOracle(p.validateOracle),
      injectOracleFault(p.injectOracleFault)
{
    workload = p.workload;
    fifo.setCapacity(p.capacity);
}

RunAheadEngine::~RunAheadEngine()
{}

void
RunAheadEngine::init()
{
    requestorId = systemPtr->getRequestorId(this);
    // Reads consult the overlay first, then fall through to a functional read
    // of base memory. In SE we read via the process's translating proxy: it
    // resolves the loaded image content and demand-allocates pages (Always),
    // which a raw mmu+dcache functional read does not. Per the memory model
    // the engine overlays its own ahead stores, so base only serves addresses
    // with no store since the last sync -- which are physical-consistent.
    overlay.setBaseReader([this](Addr vaddr, size_t n, uint8_t *out) {
        SETranslatingPortProxy proxy(tc, SETranslatingPortProxy::Always);
        proxy.readBlob(vaddr, out, n);
    });
}

void
RunAheadEngine::setSystem(System *system)
{
    const Params &p = params();
    systemPtr = system;
    if (FullSystem) {
        thread =
            new SimpleThread(this, 0, systemPtr, mmu, p.isa[0], p.decoder[0]);
    } else {
        thread = new SimpleThread(this, 0, systemPtr,
                                  workload.size() ? workload[0] : nullptr, mmu,
                                  p.isa[0], p.decoder[0]);
    }
    tc = thread->getTC();
    threadContexts.push_back(tc);
    assert(thread != nullptr);
}

void
RunAheadEngine::setIcachePort(RequestPort *icache_port)
{
    icachePort = icache_port;
}

void
RunAheadEngine::setDcachePort(RequestPort *dcache_port)
{
    dcachePort = dcache_port;
}

void
RunAheadEngine::initFromThreadContext(gem5::ThreadContext *src_tc)
{
    assert(thread);
    tc->getIsaPtr()->setThreadContext(tc);
    thread->copyArchRegs(src_tc);
    thread->pcState(src_tc->pcState());
    overlay.drop();
    curStaticInst = nullStaticInstPtr;
    curMacroStaticInst = nullStaticInstPtr;
    syscallBarrier = false;
    initialized = true;
    DPRINTF(Oracle, "RAE synced to TC, PC=%s\n", thread->pcState());
}

void
RunAheadEngine::recordDestScalar(const RegId &flat, RegVal val)
{
    if (!curOracle) {
        return;
    }
    OracleRegResult r;
    r.regClass = flat.classValue();
    r.regIndex = flat.index();
    r.value.resize(sizeof(RegVal));
    std::memcpy(r.value.data(), &val, sizeof(RegVal));
    curOracle->dests.push_back(std::move(r));
}

void
RunAheadEngine::recordDestWide(const RegId &flat)
{
    if (!curOracle) {
        return;
    }
    OracleRegResult r;
    r.regClass = flat.classValue();
    r.regIndex = flat.index();
    const size_t n = flat.regClass().regBytes();
    r.value.resize(n);
    thread->getReg(flat, r.value.data());
    curOracle->dests.push_back(std::move(r));
}

bool
RunAheadEngine::produceNext()
{
    if (syscallBarrier || fifo.full()) {
        return false;
    }

    InstDecoder *decoder = thread->decoder;
    const Addr pc_mask = decoder->pcMask();
    Fault fault = NoFault;
    curStaticInst = nullStaticInstPtr;

    // ---- Fetch + decode the next (micro-)instruction (mirrors CheckerCPU).
    // --
    uint64_t fetchOffset = 0;
    bool fetchDone = false;
    while (!fetchDone) {
        Addr fetch_PC = thread->pcState().instAddr();
        fetch_PC = (fetch_PC & pc_mask) + fetchOffset;

        if (!curMacroStaticInst) {
            auto mem_req = std::make_shared<Request>(
                fetch_PC, decoder->moreBytesSize(), 0, requestorId, fetch_PC,
                thread->contextId());
            mem_req->setVirt(fetch_PC, decoder->moreBytesSize(),
                             Request::INST_FETCH, requestorId,
                             thread->pcState().instAddr());
            fault = mmu->translateFunctional(mem_req, tc, BaseMMU::Execute);
            if (fault != NoFault) {
                break;
            }
            PacketPtr pkt = new Packet(mem_req, MemCmd::ReadReq);
            pkt->dataStatic(decoder->moreBytesPtr());
            icachePort->sendFunctional(pkt);
            delete pkt;
        }

        if (fault == NoFault) {
            std::unique_ptr<PCStateBase> pc_state(thread->pcState().clone());
            if (isRomMicroPC(pc_state->microPC())) {
                fetchDone = true;
                curStaticInst =
                    decoder->fetchRomMicroop(pc_state->microPC(), nullptr);
            } else if (!curMacroStaticInst) {
                StaticInstPtr instPtr = nullptr;
                Addr fetch_pc = (pc_state->instAddr() & pc_mask) + fetchOffset;
                decoder->moreBytes(*pc_state, fetch_pc);
                if (decoder->instReady()) {
                    fetchDone = true;
                    instPtr = decoder->decode(*pc_state);
                    thread->pcState(*pc_state);
                } else {
                    fetchOffset += decoder->moreBytesSize();
                }
                if (instPtr && instPtr->isMacroop()) {
                    curMacroStaticInst = instPtr;
                    curStaticInst = instPtr->fetchMicroop(pc_state->microPC());
                } else {
                    curStaticInst = instPtr;
                }
            } else {
                curStaticInst =
                    curMacroStaticInst->fetchMicroop(pc_state->microPC());
                fetchDone = true;
            }
        }
    }
    decoder->reset();

    // Stop (barrier) at a syscall: the timing model is the syscall authority;
    // the engine resyncs after it commits. The syscall gets no OracleInfo.
    if (fault == NoFault && curStaticInst && curStaticInst->isSyscall()) {
        syscallBarrier = true;
        DPRINTF(Oracle, "RAE barrier at syscall, PC=%s\n", thread->pcState());
        return false;
    }

    // ---- Produce the OracleInfo record and functionally execute. ----
    const Addr pc = thread->pcState().instAddr();
    const MicroPC upc = thread->pcState().microPC();
    const uint64_t idx = fifo.push(pc, upc);
    curOracle = fifo.mutableAt(idx);

    if (fault == NoFault) {
        fault = curStaticInst->execute(this, nullptr);
    }
    curOracle->faulted = (fault != NoFault);

    if (fault == NoFault && curStaticInst != nullStaticInstPtr) {
        if (curStaticInst->isLastMicroop()) {
            curMacroStaticInst = nullStaticInstPtr;
        }
        curStaticInst->advancePC(thread);
    } else {
        // v1 does not model fault delivery on the run-ahead path (SE); the
        // commit-time assert simply records that this instruction faulted.
        curMacroStaticInst = nullStaticInstPtr;
    }

    curOracle->npc = thread->pcState().instAddr();
    curOracle = nullptr;
    numInst++;
    return true;
}

const OracleInfo *
RunAheadEngine::consumeAtFetch(Addr pc, MicroPC upc)
{
    if (!enable || !initialized) {
        return nullptr;
    }

    // Produce until the next true record covers this PC, or we cannot make
    // progress (off the true path, a syscall barrier, or a full buffer).
    while (true) {
        const OracleInfo *next = fifo.peekNext();
        if (next) {
            break; // there is a next true record to match against
        }
        if (!produceNext()) {
            break; // barrier / full / cannot produce
        }
    }
    return fifo.matchAndConsume(pc, upc);
}

// ---- Memory ----

Fault
RunAheadEngine::readMem(Addr addr, uint8_t *data, unsigned size,
                        Request::Flags flags,
                        const std::vector<bool> &byte_enable)
{
    if (flags.isSet(Request::LLSC)) {
        // Load-exclusive: set the LL/SC monitor so a later store-exclusive
        // succeeds. Single-core SE has no contention, so this matches O3 and
        // keeps compare-and-swap loops on the same control-flow path. The
        // monitor is keyed on the physical address, so translate first.
        auto req = std::make_shared<Request>(addr, size, flags, requestorId,
                                             thread->pcState().instAddr(),
                                             tc->contextId());
        if (mmu->translateFunctional(req, tc, BaseMMU::Read) == NoFault) {
            thread->getIsaPtr()->handleLockedRead(req);
        }
    }
    overlay.read(addr, data, size);
    if (curOracle) {
        curOracle->isMemRef = true;
        curOracle->effAddr = addr;
        curOracle->memSize = size;
        curOracle->memData.assign(data, data + size);
    }
    return NoFault;
}

Fault
RunAheadEngine::writeMem(uint8_t *data, unsigned size, Addr addr,
                         Request::Flags flags, uint64_t *res,
                         const std::vector<bool> &byte_enable)
{
    // Store-exclusive: check the monitor (single-core SE -> succeeds, matching
    // O3). The engine never writes real memory: stores land in the overlay.
    bool do_access = true;
    const bool sc = flags.isSet(Request::LLSC);
    if (sc) {
        auto req = std::make_shared<Request>(addr, size, flags, requestorId,
                                             thread->pcState().instAddr(),
                                             tc->contextId());
        if (mmu->translateFunctional(req, tc, BaseMMU::Write) == NoFault) {
            do_access = thread->getIsaPtr()->handleLockedWrite(
                req, ~(Addr(cacheLineSize()) - 1));
        }
    }
    // ARM computes the SC status as "XResult = !writeResult", so the result
    // is 1 on success and 0 on failure (ARM only sets req extraData on
    // failure, so we derive it directly). Non-SC stores leave res at 0.
    if (res) {
        *res = (sc && do_access) ? 1 : 0;
    }
    if (do_access && data) {
        overlay.write(addr, data, size);
    }
    if (curOracle) {
        curOracle->isMemRef = true;
        curOracle->effAddr = addr;
        curOracle->memSize = size;
        if (do_access && data) {
            curOracle->memData.assign(data, data + size);
        }
    }
    return NoFault;
}

Fault
RunAheadEngine::amoMem(Addr addr, uint8_t *data, unsigned size,
                       Request::Flags flags, AtomicOpFunctorPtr amo_op)
{
    // Atomic read-modify-write on the overlay (e.g. CAS, LDADD, SWP). Read the
    // current value, return the OLD value to the instruction in @p data, apply
    // the atomic op to produce the new value, and store it in the overlay. The
    // engine never writes real memory.
    std::vector<uint8_t> mem(size);
    overlay.read(addr, mem.data(), size);
    std::memcpy(data, mem.data(), size);
    if (amo_op) {
        (*amo_op)(mem.data());
    }
    overlay.write(addr, mem.data(), size);
    if (curOracle) {
        curOracle->isMemRef = true;
        curOracle->effAddr = addr;
        curOracle->memSize = size;
        curOracle->memData.assign(mem.begin(), mem.end());
    }
    return NoFault;
}

RequestPtr
RunAheadEngine::genMemFragmentRequest(Addr frag_addr, int size,
                                      Request::Flags flags,
                                      const std::vector<bool> &byte_enable,
                                      int &frag_size, int &size_left) const
{
    // Unused in v1 (readMem/writeMem use the overlay directly); retained for
    // ExecContext-interface completeness.
    frag_size = size_left;
    size_left = 0;
    return std::make_shared<Request>(frag_addr, frag_size, flags, requestorId,
                                     thread->pcState().instAddr(),
                                     tc->contextId());
}

namespace
{

// True iff the recorded oracle value equals O3's actual result @p res.
bool
sameDestValue(const OracleRegResult &r, const InstResult &res)
{
    if (!res.isValid()) {
        return false;
    }
    if (res.isBlob()) {
        return r.value.size() == res.regClass().regBytes() &&
               std::memcmp(r.value.data(), res.asBlob(), r.value.size()) == 0;
    }
    const RegVal v = res.asRegVal();
    return r.value.size() == sizeof(RegVal) &&
           std::memcmp(r.value.data(), &v, sizeof(RegVal)) == 0;
}

} // anonymous namespace

void
RunAheadEngine::validateAtCommit(DynInst *inst)
{
    const OracleInfo *oi = inst->oracleInfo;
    const uint64_t idx = inst->oracleTrueIndex;

    if (validateOracle && oi && !inst->isUnverifiable() &&
        !inst->staticInst->isSyscall() &&
        !inst->staticInst->isStoreConditional()) {
        bool ok = true;
        std::string why;

        // A committed instruction did not fault (faults are resolved before
        // commit), so the oracle must agree.
        if (oi->faulted) {
            ok = false;
            why = "oracle marked this committed instruction as faulted";
        }

        // Destination register values, in execute order.
        const size_t nActual = inst->resultSize();
        if (ok && oi->dests.size() != nActual) {
            ok = false;
            why = csprintf("dest count: oracle=%u actual=%u", oi->dests.size(),
                           nActual);
        }
        for (size_t i = 0; ok && i < oi->dests.size(); i++) {
            InstResult res = inst->popResult();
            if (!sameDestValue(oi->dests[i], res)) {
                ok = false;
                why = csprintf("dest[%u] class=%d idx=%u actual=%s", i,
                               oi->dests[i].regClass, oi->dests[i].regIndex,
                               res.isValid() ? res.asString() : "<invalid>");
            }
        }

        // Next-PC (branch direction + target).
        if (ok) {
            std::unique_ptr<PCStateBase> npc(inst->pcState().clone());
            inst->staticInst->advancePC(*npc);
            if (npc->instAddr() != oi->npc) {
                ok = false;
                why = csprintf("next-PC: oracle=%#x actual=%#x", oi->npc,
                               npc->instAddr());
            }
        }

        panic_if(!ok, "Oracle mismatch [sn:%llu] PC %s: %s\n  oracle: %s",
                 inst->seqNum, inst->pcState(), why, oi->dump());
    }

    // Release the retained FIFO entry once the instruction commits.
    if (oi) {
        fifo.release(idx);
    }
}

void
RunAheadEngine::onSquash(DynInst *inst, bool reFetchInst)
{
    // Realign only when the squashing instruction is on the true path (it
    // carries an oracle index). Nested wrong-path squashes carry none; the
    // next correct-path squash re-aligns.
    if (!inst->hasOracleInfo()) {
        return;
    }
    fifo.rewindTo(inst->oracleTrueIndex + (reFetchInst ? 0 : 1));
}

void
RunAheadEngine::onSyscallCommit(gem5::ThreadContext *post_tc)
{
    // The timing model executed the syscall (the authority); adopt its
    // post-syscall architectural state, drop the overlay (the syscall may have
    // changed memory), clear the barrier, and resume run-ahead production.
    initFromThreadContext(post_tc);
}

} // namespace o3
} // namespace gem5
