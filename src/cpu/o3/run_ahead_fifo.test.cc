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

#include <gtest/gtest.h>

#include "cpu/o3/run_ahead_fifo.hh"

using namespace gem5;
using namespace gem5::o3;

TEST(OracleFifo, InOrderConsumeByPosition)
{
    OracleFifo f;
    f.setCapacity(8);
    f.push(0x100, 0);
    f.push(0x100, 0); // same PC as the first, different dynamic instance
    f.push(0x104, 0);

    auto *a = f.matchAndConsume(0x100, 0);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->trueIndex, 0u);
    auto *b = f.matchAndConsume(0x100, 0);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->trueIndex, 1u); // 2nd instance, identified by position
    auto *c = f.matchAndConsume(0x104, 0);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->trueIndex, 2u);
}

TEST(OracleFifo, NonMatchingPcDoesNotConsume)
{
    OracleFifo f;
    f.setCapacity(8);
    f.push(0x200, 0);

    EXPECT_EQ(f.matchAndConsume(0x999, 0), nullptr); // off the true path
    auto *a = f.matchAndConsume(0x200, 0);           // still there
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->trueIndex, 0u);
}

TEST(OracleFifo, RewindRestoresConsumePointer)
{
    OracleFifo f;
    f.setCapacity(8);
    f.push(0x300, 0);
    f.push(0x304, 0);
    f.push(0x308, 0);
    f.matchAndConsume(0x300, 0);
    f.matchAndConsume(0x304, 0);

    f.rewindTo(1); // re-fetch from index 1
    auto *a = f.matchAndConsume(0x304, 0);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->trueIndex, 1u);
}

TEST(OracleFifo, ReleasePopsCommittedTail)
{
    OracleFifo f;
    f.setCapacity(8);
    f.push(0x400, 0);
    f.push(0x404, 0);
    f.matchAndConsume(0x400, 0);
    f.matchAndConsume(0x404, 0);

    f.release(0);               // commit index 0
    EXPECT_EQ(f.pending(), 1u); // only index 1 retained
}

TEST(OracleFifo, FullThrottle)
{
    OracleFifo f;
    f.setCapacity(2);
    f.push(0x500, 0);
    f.push(0x504, 0);
    EXPECT_TRUE(f.full()); // producer must pause
}
