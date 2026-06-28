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

#include <utility>
#include <vector>

#include "cpu/o3/oracle_info.hh"

using namespace gem5;
using namespace gem5::o3;

// Register-class ids: values mirror gem5::RegClassType, but the comparator
// only needs them to be distinct, so the test avoids pulling in
// cpu/reg_class.hh.
static constexpr int kIntClass = 0;
static constexpr int kFloatClass = 1;

static OracleRegResult
reg(RegIndex idx, std::vector<uint8_t> v)
{
    return OracleRegResult{kIntClass, idx, std::move(v)};
}

TEST(OracleInfo, DestsMatchEqual)
{
    OracleInfo oi;
    oi.dests = {reg(1, {0xde, 0xad}), reg(2, {0x01})};
    std::vector<OracleRegResult> actual = {reg(1, {0xde, 0xad}),
                                           reg(2, {0x01})};
    EXPECT_TRUE(oi.destsMatch(actual));
}

TEST(OracleInfo, DestsMismatchValue)
{
    OracleInfo oi;
    oi.dests = {reg(1, {0xde, 0xad})};
    std::vector<OracleRegResult> actual = {reg(1, {0xde, 0xbe})};
    EXPECT_FALSE(oi.destsMatch(actual));
}

TEST(OracleInfo, DestsMismatchCount)
{
    OracleInfo oi;
    oi.dests = {reg(1, {0x00})};
    EXPECT_FALSE(oi.destsMatch({}));
}

TEST(OracleInfo, DestsMismatchClass)
{
    OracleInfo oi;
    oi.dests = {reg(1, {0x00})};
    std::vector<OracleRegResult> actual = {{kFloatClass, 1, {0x00}}};
    EXPECT_FALSE(oi.destsMatch(actual));
}

TEST(OracleInfo, DumpMentionsPc)
{
    OracleInfo oi;
    oi.pc = 0x400abc;
    oi.trueIndex = 7;
    auto s = oi.dump();
    EXPECT_NE(s.find("400abc"), std::string::npos);
}
