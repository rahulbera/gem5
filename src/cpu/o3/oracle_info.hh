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

#ifndef __CPU_O3_ORACLE_INFO_HH__
#define __CPU_O3_ORACLE_INFO_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "base/types.hh"

namespace gem5
{

namespace o3
{

/**
 * Ground-truth value written to one destination register by an instruction.
 *
 * Register identity is stored as (class, index) rather than a full RegId so
 * that this record (and its unit tests) carry no ISA / RegClass dependency.
 * The run-ahead engine and the commit-time validator translate to/from the
 * DynInst's RegIds via RegId::classValue() and RegId::index().
 */
struct OracleRegResult
{
    // A gem5::RegClassType value (see cpu/reg_class.hh), stored as a plain int
    // so this record carries no dependency on the reg-class machinery (and its
    // debug-flag link requirements). The engine/validator translate via
    // RegId::classValue().
    int regClass;
    RegIndex regIndex;
    std::vector<uint8_t> value;

    bool
    operator==(const OracleRegResult &o) const
    {
        return regClass == o.regClass && regIndex == o.regIndex &&
               value == o.value;
    }
};

/**
 * Ground truth for one (micro-)instruction on the true dynamic path,
 * produced by the run-ahead engine and attached to the matching DynInst at
 * fetch. In v1 this is metadata only: it is validated against the real
 * execution at commit (see the run-ahead engine), never consumed by the
 * pipeline.
 */
struct OracleInfo
{
    /** Position in the true dynamic instruction stream (the real identity). */
    uint64_t trueIndex = 0;
    Addr pc = 0;
    MicroPC upc = 0;
    /** True next instruction address (branch direction + target). */
    Addr npc = 0;
    std::vector<OracleRegResult> dests;
    bool isMemRef = false;
    Addr effAddr = 0;
    uint32_t memSize = 0;
    std::vector<uint8_t> memData;
    bool faulted = false;

    /** True iff the recorded destination writes equal @p actual, in order. */
    bool destsMatch(const std::vector<OracleRegResult> &actual) const;

    /** One-line human-readable summary for diagnostics. */
    std::string dump() const;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_ORACLE_INFO_HH__
