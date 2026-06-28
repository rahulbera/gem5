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

#ifndef __CPU_O3_RUN_AHEAD_FIFO_HH__
#define __CPU_O3_RUN_AHEAD_FIFO_HH__

#include <cstddef>
#include <cstdint>
#include <deque>

#include "base/types.hh"
#include "cpu/o3/oracle_info.hh"

namespace gem5
{

namespace o3
{

/**
 * In-order buffer of OracleInfo records keyed by trueIndex (the record's
 * position in the true dynamic instruction stream -- its real identity, since
 * a PC may repeat across loop iterations). Records live from production until
 * release() at commit. `consume` is the fetch frontier; `tail` is the oldest
 * unreleased index. Fetch matches strictly against the single next record, so
 * repeated PCs are disambiguated by position, never by value.
 */
class OracleFifo
{
  public:
    void
    setCapacity(size_t c)
    {
        capacity = c;
    }
    bool
    full() const
    {
        return buf.size() >= capacity;
    }
    size_t
    pending() const
    {
        return buf.size();
    }
    uint64_t
    nextProduceIndex() const
    {
        return tail + buf.size();
    }

    /** Producer: append a record for the next true index; returns its index.
     */
    uint64_t push(Addr pc, MicroPC upc);

    /** The next record the fetch frontier expects (nullptr if none yet). */
    const OracleInfo *peekNext() const;

    /**
     * If (pc, upc) matches the next record, advance the consume frontier and
     * return it (still retained until release); else return nullptr (the fetch
     * is off the true path).
     */
    const OracleInfo *matchAndConsume(Addr pc, MicroPC upc);

    /** Mutable access by index, so the engine can fill results after push. */
    OracleInfo *mutableAt(uint64_t trueIndex);

    /** Rewind the consume frontier (e.g. on a correct-path squash). */
    void
    rewindTo(uint64_t trueIndex)
    {
        consume = trueIndex;
    }

    /** Drop all records with trueIndex <= committedTrueIndex. */
    void release(uint64_t committedTrueIndex);

  private:
    std::deque<OracleInfo> buf; // buf[i] has trueIndex == tail + i
    uint64_t tail = 0;          // trueIndex of buf.front()
    uint64_t consume = 0;       // next index fetch will try to consume
    size_t capacity = 256;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RUN_AHEAD_FIFO_HH__
