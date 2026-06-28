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

#include "cpu/o3/run_ahead_fifo.hh"

#include <utility>

namespace gem5
{

namespace o3
{

uint64_t
OracleFifo::push(Addr pc, MicroPC upc)
{
    uint64_t idx = tail + buf.size();
    OracleInfo oi;
    oi.trueIndex = idx;
    oi.pc = pc;
    oi.upc = upc;
    buf.push_back(std::move(oi));
    return idx;
}

const OracleInfo *
OracleFifo::peekNext() const
{
    if (consume < tail || consume >= tail + buf.size()) {
        return nullptr;
    }
    return &buf[consume - tail];
}

const OracleInfo *
OracleFifo::matchAndConsume(Addr pc, MicroPC upc)
{
    const OracleInfo *n = peekNext();
    if (!n || n->pc != pc || n->upc != upc) {
        return nullptr;
    }
    ++consume;
    return n;
}

OracleInfo *
OracleFifo::mutableAt(uint64_t trueIndex)
{
    if (trueIndex < tail || trueIndex >= tail + buf.size()) {
        return nullptr;
    }
    return &buf[trueIndex - tail];
}

void
OracleFifo::release(uint64_t committedTrueIndex)
{
    while (!buf.empty() && tail <= committedTrueIndex) {
        buf.pop_front();
        ++tail;
    }
    if (consume < tail) {
        consume = tail;
    }
}

} // namespace o3
} // namespace gem5
