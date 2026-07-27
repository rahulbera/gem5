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

TEST(MrnValueFileTables, RebindClearsSelfBound)
{
    MrnValueFileTables t(testConfig());

    // The load self-binds (nothing published yet) and writes back a
    // last-value.
    auto probe1 = t.loadAddrResolved(0x200, 0x2000);
    ASSERT_EQ(probe1.outcome, MrnVfProbeResult::SelfBound);
    ASSERT_TRUE(t.loadDataResolved(0x200, 111));

    // A store now publishes that same address; the load's next probe
    // must rebind onto the store's channel, clearing selfBound.
    MrnVfRef refStore = t.storeRename(0x100, fakeReg(7), 10, nullptr);
    ASSERT_TRUE(t.storeAddrResolved(refStore, 0x2000, 10));
    auto probe2 = t.loadAddrResolved(0x200, 0x2000);
    EXPECT_EQ(probe2.outcome, MrnVfProbeResult::Rebound);

    // No longer self-bound: a late writeback value must not be accepted.
    EXPECT_FALSE(t.loadDataResolved(0x200, 222));

    // A different, freshly self-bound load elsewhere is unaffected.
    auto probe3 = t.loadAddrResolved(0x300, 0x3000);
    ASSERT_EQ(probe3.outcome, MrnVfProbeResult::SelfBound);
    EXPECT_TRUE(t.loadDataResolved(0x300, 333));
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
    // Generous SLC capacity (assoc 4, vs. the default 2) so the load's
    // SLC entry can never be capacity-evicted by the thief stores below;
    // vfEntries stays at 4 so stealing all four VF cells is still what
    // forces the mismatch this test targets: loadRename must hit the
    // `valueFile[e->vfIdx].gen != e->gen` check (mem_rename_valuefile.cc,
    // loadRename), not a plain SLC miss.
    MrnVfConfig cfg = testConfig();
    cfg.slcEntries = 16;
    cfg.slcAssoc = 4;
    MrnValueFileTables t(cfg);
    bindAndTrain(t, 0x100, 0x200, 0x1000, fakeReg(7), 10);
    ASSERT_TRUE(t.loadRename(0x200).bound);

    // Four stores at PCs outside the load's SLC set steal all four VF
    // cells (global LRU) without ever touching the load's SLC entry, so
    // it stays present throughout -- only its cell's generation moves.
    const Addr thief_pcs[] = {0x501, 0x502, 0x503, 0x505};
    for (int i = 0; i < 3; i++) {
        t.storeRename(thief_pcs[i], fakeReg(20 + i), 100 + i, nullptr);
        // The load's own cell is not yet the global LRU-min victim: it
        // survives each of the first three thefts.
        ASSERT_TRUE(t.loadRename(0x200).bound);
    }
    t.storeRename(thief_pcs[3], fakeReg(23), 103, nullptr);
    // The fourth theft finally reallocates the load's own cell. Its SLC
    // entry is still present (confirmed above), so this miss is a
    // genuine gen mismatch, not an SLC miss.
    EXPECT_FALSE(t.loadRename(0x200).bound);
}

TEST(MrnValueFileTables, StoreRenameRepairsStolenCell)
{
    MrnValueFileTables t(testConfig()); // vfEntries = 4
    const Addr store_a = 0x100;
    MrnVfRef refA1 = t.storeRename(store_a, fakeReg(7), 10, nullptr);
    ASSERT_TRUE(refA1.valid());

    // Steal all four VF cells with other stores at PCs outside A's SLC
    // set, so A's SLC entry survives and still points at refA1.idx when
    // A deposits again below.
    const Addr thief_pcs[] = {0x501, 0x502, 0x503, 0x505};
    for (int i = 0; i < 4; i++) {
        t.storeRename(thief_pcs[i], fakeReg(20 + i), 100 + i, nullptr);
    }

    // A deposits again: its SLC entry still points at cell refA1.idx,
    // but that cell's generation has moved on. This must hit the repair
    // branch in storeRename (mem_rename_valuefile.cc) -- a fresh
    // vfAllocate rather than depositing into someone else's live cell.
    MrnVfRef refA2 = t.storeRename(store_a, fakeReg(77), 20, nullptr);
    ASSERT_TRUE(refA2.valid());
    EXPECT_FALSE(refA2 == refA1); // a genuinely different, fresh cell

    // A subsequent bind+consume must see the NEW deposit's pointer, not
    // whatever the repaired-away cell used to hold.
    ASSERT_TRUE(t.storeAddrResolved(refA2, 0x1000, 20));
    auto probe = t.loadAddrResolved(0x200, 0x1000);
    EXPECT_EQ(probe.outcome, MrnVfProbeResult::Rebound);
    auto r = t.loadRename(0x200);
    ASSERT_TRUE(r.bound);
    EXPECT_TRUE(r.ptrValid);
    EXPECT_EQ(r.ptr, fakeReg(77));
    EXPECT_EQ(r.ptrSeq, 20u);
    EXPECT_TRUE(r.ref == refA2);
}

TEST(MrnValueFileTables, ProbeDeadChannelSelfBinds)
{
    MrnValueFileTables t(testConfig()); // vfEntries = 4
    MrnVfRef refA = t.storeRename(0x100, fakeReg(7), 10, nullptr);
    ASSERT_TRUE(t.storeAddrResolved(refA, 0x1000, 10));

    // Steal every VF cell, including A's, with other stores: the store
    // cache entry published above now points at a reallocated cell.
    for (int i = 0; i < 4; i++) {
        t.storeRename(0x300 + 8 * i, fakeReg(20 + i), 100 + i, nullptr);
    }

    // A load probing that address hits the SC entry but finds its cell's
    // generation has moved on: dead channel, so it falls through to the
    // self-bind path instead of rebinding onto a stale channel.
    auto probe = t.loadAddrResolved(0x200, 0x1000);
    EXPECT_TRUE(probe.scHitDeadChannel);
    EXPECT_EQ(probe.outcome, MrnVfProbeResult::SelfBound);
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

TEST(MrnValueFileTables, AddressChangeObservation)
{
    MrnValueFileTables t(testConfig());
    ASSERT_EQ(t.loadAddrResolved(0x200, 0x2000).outcome,
              MrnVfProbeResult::SelfBound);
    MrnVfRef ref = t.loadRename(0x200).ref;
    ASSERT_TRUE(ref.valid());
    EXPECT_FALSE(t.cellAddrChanged(ref)); // first instance: no change
    t.loadAddrResolved(0x200, 0x2000);    // same line
    EXPECT_FALSE(t.cellAddrChanged(ref));
    t.loadAddrResolved(0x200, 0x3000); // different line
    EXPECT_TRUE(t.cellAddrChanged(ref));
    t.loadAddrResolved(0x200, 0x3000); // stable at the new line
    EXPECT_FALSE(t.cellAddrChanged(ref));
}

TEST(MrnValueFileTables, StrikeHysteresisDisablesAndReenables)
{
    MrnVfConfig c = testConfig();
    c.lvStabilityTarget = 3;
    MrnValueFileTables t(c);
    ASSERT_EQ(t.loadAddrResolved(0x200, 0x2000).outcome,
              MrnVfProbeResult::SelfBound);
    ASSERT_TRUE(t.loadDataResolved(0x200, 7));
    MrnVfRef ref = t.loadRename(0x200).ref;
    // Wrong at a STABLE address never strikes.
    t.trainVerify(0x200, ref, false, /*addrChanged=*/false);
    EXPECT_FALSE(t.loadRename(0x200).lvProbation);
    // First addr-changed wrong: one strike, still enabled.
    t.trainVerify(0x200, ref, false, /*addrChanged=*/true);
    EXPECT_FALSE(t.loadRename(0x200).lvProbation);
    // Second: sticky disable.
    t.trainVerify(0x200, ref, false, /*addrChanged=*/true);
    EXPECT_TRUE(t.lastStrikeDisabled());
    EXPECT_TRUE(t.loadRename(0x200).lvProbation);
    // Corrects do NOT re-enable a disabled binding (no oscillation)...
    for (int i = 0; i < 200; i++) {
        t.trainVerify(0x200, ref, true);
    }
    EXPECT_TRUE(t.loadRename(0x200).lvProbation);
    // ...but sustained address stability at execute does.
    t.loadAddrResolved(0x200, 0x2000); // establishes line, streak 1
    t.loadAddrResolved(0x200, 0x2000); // streak 2
    EXPECT_TRUE(t.loadRename(0x200).lvProbation);
    t.loadAddrResolved(0x200, 0x2000); // streak 3 -> re-enable
    EXPECT_FALSE(t.loadRename(0x200).lvProbation);
    // A line change while disabled resets the streak.
    t.trainVerify(0x200, ref, false, true);
    t.trainVerify(0x200, ref, false, true);
    ASSERT_TRUE(t.loadRename(0x200).lvProbation);
    t.loadAddrResolved(0x200, 0x2000);
    t.loadAddrResolved(0x200, 0x3000); // change -> streak reset
    t.loadAddrResolved(0x200, 0x3000);
    t.loadAddrResolved(0x200, 0x3000);
    EXPECT_TRUE(t.loadRename(0x200).lvProbation); // only streak 2 at 0x3000
    t.loadAddrResolved(0x200, 0x3000);
    EXPECT_FALSE(t.loadRename(0x200).lvProbation);
}

TEST(MrnValueFileTables, StrikeHysteresisDisabledByDefault)
{
    MrnValueFileTables t(testConfig()); // lvStabilityTarget = 0
    ASSERT_EQ(t.loadAddrResolved(0x200, 0x2000).outcome,
              MrnVfProbeResult::SelfBound);
    ASSERT_TRUE(t.loadDataResolved(0x200, 7));
    MrnVfRef ref = t.loadRename(0x200).ref;
    t.trainVerify(0x200, ref, false, true);
    t.trainVerify(0x200, ref, false, true);
    EXPECT_FALSE(t.lastStrikeDisabled());
    EXPECT_FALSE(t.loadRename(0x200).lvProbation);
}

TEST(MrnValueFileTables, EarningBindingSurvivesBurstyStrikes)
{
    MrnVfConfig c = testConfig();
    c.lvStabilityTarget = 3;
    MrnValueFileTables t(c);
    ASSERT_EQ(t.loadAddrResolved(0x200, 0x2000).outcome,
              MrnVfProbeResult::SelfBound);
    ASSERT_TRUE(t.loadDataResolved(0x200, 7));
    MrnVfRef ref = t.loadRename(0x200).ref;
    // Earn a long correct history (ratio >> 256 per wrong).
    for (int i = 0; i < 600; i++) {
        t.trainVerify(0x200, ref, true);
    }
    // A clustered burst of two address-changed wrongs must NOT disable:
    // lifetime earning is 600 corrects / 2 wrongs >= 256.
    t.trainVerify(0x200, ref, false, true);
    t.trainVerify(0x200, ref, false, true);
    EXPECT_FALSE(t.loadRename(0x200).lvProbation);
    // A genuine churner (few corrects per wrong) still gets disabled.
    ASSERT_EQ(t.loadAddrResolved(0x300, 0x9000).outcome,
              MrnVfProbeResult::SelfBound);
    ASSERT_TRUE(t.loadDataResolved(0x300, 5));
    MrnVfRef r2 = t.loadRename(0x300).ref;
    for (int i = 0; i < 20; i++) {
        t.trainVerify(0x300, r2, true);
    }
    t.trainVerify(0x300, r2, false, true);
    t.trainVerify(0x300, r2, false, true);
    EXPECT_TRUE(t.loadRename(0x300).lvProbation);
}
