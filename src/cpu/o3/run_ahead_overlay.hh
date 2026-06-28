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

#ifndef __CPU_O3_RUN_AHEAD_OVERLAY_HH__
#define __CPU_O3_RUN_AHEAD_OVERLAY_HH__

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

#include "base/types.hh"

namespace gem5
{

namespace o3
{

/**
 * Copy-on-write byte overlay over a base memory, used by the run-ahead engine
 * to hold its own ahead-of-commit stores without disturbing the timing model's
 * memory. Reads consult the overlay per byte and fall through to the supplied
 * base reader on a miss; writes land only in the overlay. Byte granularity
 * makes partial-overlap reads correct with no special cases.
 */
class RunAheadStoreOverlay
{
  public:
    /** Reads @p n bytes of base memory at @p addr into the caller's buffer. */
    using BaseReader = std::function<void(Addr, size_t, uint8_t *)>;

    void
    setBaseReader(BaseReader r)
    {
        baseReader = std::move(r);
    }

    /** Record an ahead store of @p n bytes from @p data at @p addr. */
    void write(Addr addr, const uint8_t *data, size_t n);

    /** Read @p n bytes at @p addr: overlay where present, else base. */
    void read(Addr addr, uint8_t *out, size_t n) const;

    /** Forget all overlaid stores (e.g. at a syscall resync point). */
    void
    drop()
    {
        bytes.clear();
    }

    /** Number of overlaid bytes (for tests / bounding). */
    size_t
    size() const
    {
        return bytes.size();
    }

  private:
    std::unordered_map<Addr, uint8_t> bytes;
    BaseReader baseReader;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RUN_AHEAD_OVERLAY_HH__
