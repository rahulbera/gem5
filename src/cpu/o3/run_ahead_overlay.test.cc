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

#include <algorithm>

#include "cpu/o3/run_ahead_overlay.hh"

using namespace gem5;
using namespace gem5::o3;

// A base memory that returns a constant fill byte for every address.
static RunAheadStoreOverlay::BaseReader
constBase(uint8_t fill)
{
    return [fill](Addr, size_t n, uint8_t *out) {
        std::fill(out, out + n, fill);
    };
}

TEST(Overlay, ReadMissFallsToBase)
{
    RunAheadStoreOverlay ov;
    ov.setBaseReader(constBase(0xBB));
    uint8_t buf[4] = {};
    ov.read(0x1000, buf, 4);
    EXPECT_EQ(buf[0], 0xBB);
    EXPECT_EQ(buf[3], 0xBB);
}

TEST(Overlay, WriteThenReadReturnsOverlay)
{
    RunAheadStoreOverlay ov;
    ov.setBaseReader(constBase(0xBB));
    uint8_t w[2] = {0xDE, 0xAD};
    ov.write(0x2000, w, 2);
    uint8_t buf[2] = {};
    ov.read(0x2000, buf, 2);
    EXPECT_EQ(buf[0], 0xDE);
    EXPECT_EQ(buf[1], 0xAD);
}

TEST(Overlay, PartialOverlapMixesOverlayAndBase)
{
    RunAheadStoreOverlay ov;
    ov.setBaseReader(constBase(0xBB));
    uint8_t w[1] = {0x11};
    ov.write(0x3001, w, 1); // only the middle byte
    uint8_t buf[3] = {};
    ov.read(0x3000, buf, 3);
    EXPECT_EQ(buf[0], 0xBB);
    EXPECT_EQ(buf[1], 0x11);
    EXPECT_EQ(buf[2], 0xBB);
}

TEST(Overlay, LastWriteWins)
{
    RunAheadStoreOverlay ov;
    ov.setBaseReader(constBase(0xBB));
    uint8_t a[1] = {0x01};
    ov.write(0x4000, a, 1);
    uint8_t b[1] = {0x02};
    ov.write(0x4000, b, 1);
    uint8_t buf[1] = {};
    ov.read(0x4000, buf, 1);
    EXPECT_EQ(buf[0], 0x02);
}

TEST(Overlay, DropClearsOverlay)
{
    RunAheadStoreOverlay ov;
    ov.setBaseReader(constBase(0xBB));
    uint8_t w[1] = {0x55};
    ov.write(0x5000, w, 1);
    ov.drop();
    uint8_t buf[1] = {};
    ov.read(0x5000, buf, 1);
    EXPECT_EQ(buf[0], 0xBB);
    EXPECT_EQ(ov.size(), 0u);
}
