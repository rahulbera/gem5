#include <gtest/gtest.h>

#include "cpu/o3/mem_rename_valuefile.hh"

using namespace gem5;
using namespace gem5::o3;

namespace
{

MrnVfConfig
testConfig()
{
    MrnVfConfig c;
    c.vfEntries = 4;
    c.slcEntries = 8;
    c.slcAssoc = 2;
    c.scEntries = 8;
    c.scAssoc = 2;
    c.scGranularityBytes = 8;
    c.confBits = 2;
    c.confThreshold = 2;
    c.confInc = 1;
    c.confDec = 1;
    c.resetConfOnMispredict = true;
    return c;
}

PhysRegIdPtr
fakeReg(uintptr_t n)
{
    return reinterpret_cast<PhysRegIdPtr>(n);
}

// Establish a confident store->load binding: deposit, publish, probe
// (rebind), then train to threshold.
MrnVfRef
bindAndTrain(MrnValueFileTables &t, Addr store_pc, Addr load_pc, Addr ea,
             PhysRegIdPtr reg, InstSeqNum sn)
{
    MrnVfRef ref = t.storeRename(store_pc, reg, sn, nullptr);
    EXPECT_TRUE(t.storeAddrResolved(ref, ea, sn));
    auto probe = t.loadAddrResolved(load_pc, ea);
    EXPECT_EQ(probe.outcome, MrnVfProbeResult::Rebound);
    for (int i = 0; i < 2; i++) {
        t.trainVerify(load_pc, ref, true);
    }
    return ref;
}

} // namespace

TEST(MrnValueFileTables, ColdLoadRenameUnbound)
{
    MrnValueFileTables t(testConfig());
    auto r = t.loadRename(0x400);
    EXPECT_FALSE(r.bound);
    EXPECT_FALSE(r.ref.valid());
}

TEST(MrnValueFileTables, DepositBindTrainConsume)
{
    MrnValueFileTables t(testConfig());
    MrnVfRef ref = bindAndTrain(t, 0x100, 0x200, 0x1000, fakeReg(7), 10);
    auto r = t.loadRename(0x200);
    EXPECT_TRUE(r.bound);
    EXPECT_TRUE(r.confident);
    EXPECT_TRUE(r.ptrValid);
    EXPECT_EQ(r.ptr, fakeReg(7));
    EXPECT_EQ(r.ptrSeq, 10u);
    EXPECT_TRUE(r.ref == ref);
}

TEST(MrnValueFileTables, DepositClearsStaleValue)
{
    MrnValueFileTables t(testConfig());
    RegVal v1 = 42;
    t.storeRename(0x100, fakeReg(7), 10, &v1);     // value deposited
    t.storeRename(0x100, fakeReg(8), 20, nullptr); // next instance: no
    // Bind a load so we can observe the cell.
    MrnVfRef ref{0, 0};
    ref = t.storeRename(0x100, fakeReg(9), 30, nullptr);
    ASSERT_TRUE(t.storeAddrResolved(ref, 0x1000, 30));
    t.loadAddrResolved(0x200, 0x1000);
    for (int i = 0; i < 2; i++) {
        t.trainVerify(0x200, ref, true);
    }
    auto r = t.loadRename(0x200);
    ASSERT_TRUE(r.bound);
    EXPECT_TRUE(r.ptrValid);
    EXPECT_FALSE(r.valueValid); // stale value must not survive deposits
}

TEST(MrnValueFileTables, RebindResetsConfidence)
{
    MrnValueFileTables t(testConfig());
    bindAndTrain(t, 0x100, 0x200, 0x1000, fakeReg(7), 10);
    ASSERT_TRUE(t.loadRename(0x200).confident);
    // A different store PC (its own cell) publishes the same address,
    // program-order younger.
    MrnVfRef refB = t.storeRename(0x300, fakeReg(9), 50, nullptr);
    ASSERT_TRUE(t.storeAddrResolved(refB, 0x1000, 50));
    auto probe = t.loadAddrResolved(0x200, 0x1000);
    EXPECT_EQ(probe.outcome, MrnVfProbeResult::Rebound);
    auto r = t.loadRename(0x200);
    EXPECT_TRUE(r.bound);
    EXPECT_FALSE(r.confident);    // rebind reset confidence
    EXPECT_EQ(r.ptr, fakeReg(9)); // now the new channel
}

TEST(MrnValueFileTables, SelfBindLastValue)
{
    MrnValueFileTables t(testConfig());
    auto probe = t.loadAddrResolved(0x200, 0x2000); // nothing published
    EXPECT_EQ(probe.outcome, MrnVfProbeResult::SelfBound);
    EXPECT_TRUE(t.loadDataResolved(0x200, 77));
    auto r0 = t.loadRename(0x200);
    ASSERT_TRUE(r0.bound);
    for (int i = 0; i < 2; i++) {
        t.trainVerify(0x200, r0.ref, true);
    }
    auto r = t.loadRename(0x200);
    ASSERT_TRUE(r.bound);
    EXPECT_TRUE(r.confident);
    EXPECT_TRUE(r.valueValid);
    EXPECT_EQ(r.value, 77u);
    EXPECT_FALSE(r.ptrValid);
    EXPECT_EQ(t.loadAddrResolved(0x200, 0x2000).outcome,
              MrnVfProbeResult::AlreadySelfBound);
}

TEST(MrnValueFileTables, GenMismatchAfterReallocation)
{
    MrnValueFileTables t(testConfig()); // vfEntries = 4
    bindAndTrain(t, 0x100, 0x200, 0x1000, fakeReg(7), 10);
    // Exhaust the value file: four more stores steal all four cells.
    for (int i = 0; i < 4; i++) {
        t.storeRename(0x300 + 8 * i, fakeReg(20 + i), 100 + i, nullptr);
    }
    auto r = t.loadRename(0x200);
    EXPECT_FALSE(r.bound); // the load's cell was reallocated: dead channel
}

TEST(MrnValueFileTables, ScProgramOrderGuard)
{
    MrnValueFileTables t(testConfig());
    MrnVfRef refA = t.storeRename(0x100, fakeReg(7), 10, nullptr);
    ASSERT_TRUE(t.storeAddrResolved(refA, 0x1000, 10));
    MrnVfRef refB = t.storeRename(0x300, fakeReg(9), 5, nullptr);
    // B is program-order OLDER (sn 5 < 10) but resolves later: suppressed.
    EXPECT_FALSE(t.storeAddrResolved(refB, 0x1000, 5));
    t.loadAddrResolved(0x200, 0x1000);
    auto r0 = t.loadRename(0x200);
    ASSERT_TRUE(r0.bound);
    EXPECT_EQ(r0.ptr, fakeReg(7)); // still A's channel
}

TEST(MrnValueFileTables, StaleRefPublishSuppressed)
{
    MrnValueFileTables t(testConfig());
    MrnVfRef ref = t.storeRename(0x100, fakeReg(7), 10, nullptr);
    for (int i = 0; i < 4; i++) { // steal every cell
        t.storeRename(0x300 + 8 * i, fakeReg(20 + i), 100 + i, nullptr);
    }
    EXPECT_FALSE(t.storeAddrResolved(ref, 0x1000, 10));
}

TEST(MrnValueFileTables, WrongTrainResetsAndStaleTrainIgnored)
{
    MrnValueFileTables t(testConfig());
    MrnVfRef ref = bindAndTrain(t, 0x100, 0x200, 0x1000, fakeReg(7), 10);
    ASSERT_TRUE(t.loadRename(0x200).confident);
    t.trainVerify(0x200, ref, false);
    EXPECT_FALSE(t.loadRename(0x200).confident);
    // Training against a reference the load is no longer bound to is a
    // no-op: rebind to another channel first.
    MrnVfRef refB = t.storeRename(0x300, fakeReg(9), 50, nullptr);
    ASSERT_TRUE(t.storeAddrResolved(refB, 0x1000, 50));
    t.loadAddrResolved(0x200, 0x1000);
    t.trainVerify(0x200, ref, true); // stale usedRef
    EXPECT_FALSE(t.loadRename(0x200).confident);
}

TEST(MrnValueFileTables, SameBindingProbeKeepsConfidence)
{
    MrnValueFileTables t(testConfig());
    MrnVfRef ref = bindAndTrain(t, 0x100, 0x200, 0x1000, fakeReg(7), 10);
    // Re-publish (next instance) and re-probe: same channel, conf kept.
    ASSERT_TRUE(t.storeAddrResolved(ref, 0x1000, 60));
    EXPECT_EQ(t.loadAddrResolved(0x200, 0x1000).outcome,
              MrnVfProbeResult::SameBinding);
    EXPECT_TRUE(t.loadRename(0x200).confident);
}
