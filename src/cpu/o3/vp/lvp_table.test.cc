#include <gtest/gtest.h>

#include "cpu/o3/vp/lvp_table.hh"
#include "cpu/o3/vp/vp_key.hh"

using namespace gem5;
using namespace gem5::o3;

namespace
{

// Small geometry: 4 sets x 2 ways, 2-bit confidence, threshold 2.
LvpConfig
testConfig()
{
    LvpConfig c;
    c.entries = 8;
    c.assoc = 2;
    c.confBits = 2;
    c.confThreshold = 2;
    c.confDecrementOnWrong = false;
    return c;
}

// Train the same (key, value) pair n times.
void
trainN(LvpTable &t, Addr key, RegVal val, int n)
{
    for (int i = 0; i < n; i++) {
        t.train(key, val);
    }
}

} // namespace

TEST(LvpTable, ColdLookupMisses)
{
    LvpTable t(testConfig());
    auto r = t.lookup(0x400);
    EXPECT_FALSE(r.hit);
    EXPECT_FALSE(r.confident);
}

TEST(LvpTable, FirstTrainAllocatesUnconfident)
{
    LvpTable t(testConfig());
    EXPECT_EQ(t.train(0x400, 42), LvpTrainOutcome::Allocated);
    auto r = t.lookup(0x400);
    EXPECT_TRUE(r.hit);
    EXPECT_FALSE(r.confident); // conf starts at 0, threshold is 2
    EXPECT_EQ(r.value, 42u);
}

TEST(LvpTable, ConfidenceBuildsToThreshold)
{
    LvpTable t(testConfig());
    t.train(0x400, 42);                      // alloc, conf = 0
    EXPECT_EQ(t.train(0x400, 42), LvpTrainOutcome::Match); // conf = 1
    EXPECT_FALSE(t.lookup(0x400).confident);
    EXPECT_EQ(t.train(0x400, 42), LvpTrainOutcome::Match); // conf = 2
    auto r = t.lookup(0x400);
    EXPECT_TRUE(r.confident); // conf == threshold predicts
    EXPECT_EQ(r.value, 42u);
}

TEST(LvpTable, MismatchResetsConfidenceAndUpdatesValue)
{
    LvpTable t(testConfig());
    trainN(t, 0x400, 42, 4); // confident (saturated at 3)
    ASSERT_TRUE(t.lookup(0x400).confident);
    EXPECT_EQ(t.train(0x400, 99), LvpTrainOutcome::MismatchReset);
    auto r = t.lookup(0x400);
    EXPECT_TRUE(r.hit);
    EXPECT_FALSE(r.confident); // reset to zero
    EXPECT_EQ(r.value, 99u);   // last value always updated
}

TEST(LvpTable, MismatchDecrementMode)
{
    LvpConfig c = testConfig();
    c.confDecrementOnWrong = true;
    LvpTable t(c);
    trainN(t, 0x400, 42, 4); // conf saturated at 3 (> threshold 2)
    EXPECT_EQ(t.train(0x400, 99), LvpTrainOutcome::MismatchDecrement);
    // No-livelock invariant: ONE mismatch must land below threshold.
    // Clamp: conf = min(3 - 1, threshold - 1) = 1 -- NOT the plain
    // decrement's 2, which would still predict.
    auto r = t.lookup(0x400);
    EXPECT_TRUE(r.hit);
    EXPECT_FALSE(r.confident);
    EXPECT_EQ(r.value, 99u); // last value always updated
    // Pin conf == 1 exactly: one match reaches the threshold (1+1=2);
    // a clamp to 0 would still be below it here, a clamp to 2 would
    // have been confident above.
    EXPECT_EQ(t.train(0x400, 99), LvpTrainOutcome::Match);
    EXPECT_TRUE(t.lookup(0x400).confident);
}

TEST(LvpTable, ConfidenceSaturatesAtCounterMax)
{
    LvpTable t(testConfig());
    trainN(t, 0x400, 42, 100); // way past 2-bit max (3)
    ASSERT_TRUE(t.lookup(0x400).confident);
    // One wrong resets from saturation; two rights get back to threshold.
    t.train(0x400, 7);
    EXPECT_FALSE(t.lookup(0x400).confident);
    trainN(t, 0x400, 7, 2);
    EXPECT_TRUE(t.lookup(0x400).confident);
}

TEST(LvpTable, LruEvictionWithinSet)
{
    LvpTable t(testConfig()); // 4 sets x 2 ways; index = (key >> 2) & 3
    // Three keys in set 0: 0x000, 0x010, 0x020 (bits [3:2] == 0).
    EXPECT_EQ(t.train(0x000, 1), LvpTrainOutcome::Allocated); // way A
    EXPECT_EQ(t.train(0x010, 2), LvpTrainOutcome::Allocated); // way B
    // Displacing a valid entry reports Evicted (the thrash signal).
    EXPECT_EQ(t.train(0x020, 3), LvpTrainOutcome::Evicted); // evicts 0x000
    EXPECT_FALSE(t.lookup(0x000).hit);
    EXPECT_TRUE(t.lookup(0x010).hit);
    EXPECT_TRUE(t.lookup(0x020).hit);
    // Training 0x010 refreshes it; the next alloc evicts 0x020.
    t.train(0x010, 2);
    t.train(0x030, 4);
    EXPECT_TRUE(t.lookup(0x010).hit);
    EXPECT_FALSE(t.lookup(0x020).hit);
}

TEST(LvpTable, LookupDoesNotTouchLru)
{
    LvpTable t(testConfig());
    t.train(0x000, 1);
    t.train(0x010, 2);
    // Lookups on 0x000 must not refresh it: it is still the LRU victim.
    (void)t.lookup(0x000);
    (void)t.lookup(0x000);
    t.train(0x020, 3);
    EXPECT_FALSE(t.lookup(0x000).hit);
    EXPECT_TRUE(t.lookup(0x010).hit);
}

TEST(LvpTable, WrongThenRetrainRecovers)
{
    LvpTable t(testConfig());
    trainN(t, 0x400, 42, 3);
    t.train(0x400, 99); // reset
    trainN(t, 0x400, 99, 2);
    auto r = t.lookup(0x400);
    EXPECT_TRUE(r.confident);
    EXPECT_EQ(r.value, 99u);
}

TEST(LvpTable, EvictionAllocatesUnconfident)
{
    LvpTable t(testConfig());
    trainN(t, 0x000, 42, 4);            // confident, conf saturated
    t.train(0x010, 2);                  // way B; 0x000 is now LRU
    // Evicting the CONFIDENT 0x000 must report Evicted...
    EXPECT_EQ(t.train(0x020, 3), LvpTrainOutcome::Evicted);
    auto r = t.lookup(0x020);
    EXPECT_TRUE(r.hit);
    EXPECT_FALSE(r.confident);          // no stale-confidence carryover
    t.train(0x020, 3);                  // one match: conf 1 < threshold 2
    EXPECT_FALSE(t.lookup(0x020).confident);
}

TEST(LvpTable, DecrementMismatchClampsBelowThreshold)
{
    // Wide counter, threshold far below saturation: a single mismatch
    // must still cross below the threshold (clamp), not drift down by
    // one from saturation as a plain decrement would.
    LvpConfig c = testConfig();
    c.confBits = 4; // counter max 15
    c.confThreshold = 2;
    c.confDecrementOnWrong = true;
    LvpTable t(c);
    trainN(t, 0x400, 42, 20); // conf saturated at 15 (>> threshold 2)
    ASSERT_TRUE(t.lookup(0x400).confident);
    EXPECT_EQ(t.train(0x400, 99), LvpTrainOutcome::MismatchDecrement);
    // conf = min(15 - 1, threshold - 1) = 1 < 2: must not predict.
    EXPECT_FALSE(t.lookup(0x400).confident);
    // Exactly one match short of the threshold after the clamp.
    EXPECT_EQ(t.train(0x400, 99), LvpTrainOutcome::Match);
    EXPECT_TRUE(t.lookup(0x400).confident);
}

TEST(LvpTable, DecrementFloorsAtZero)
{
    LvpConfig c = testConfig();
    c.confDecrementOnWrong = true;
    LvpTable t(c);
    t.train(0x400, 42);                 // alloc, conf 0
    EXPECT_EQ(t.train(0x400, 99), LvpTrainOutcome::MismatchDecrement);
    EXPECT_TRUE(t.lookup(0x400).hit);
    EXPECT_FALSE(t.lookup(0x400).confident); // wrapped counter would be
                                              // confident
    trainN(t, 0x400, 99, 2);
    EXPECT_TRUE(t.lookup(0x400).confident);  // recovers normally
}

TEST(LvpTable, AllocationRefreshesLru)
{
    LvpTable t(testConfig());
    t.train(0x000, 1);
    t.train(0x010, 2);
    t.train(0x020, 3);                  // evicts 0x000; 0x020 is newest
    t.train(0x030, 4);                  // victim must be 0x010, NOT 0x020
    EXPECT_FALSE(t.lookup(0x010).hit);
    EXPECT_TRUE(t.lookup(0x020).hit);
    EXPECT_TRUE(t.lookup(0x030).hit);
}

TEST(VpKey, MicroPcDistinguishesCrackedMicroOps)
{
    const Addr pc = 0x400890;
    EXPECT_NE(vpKey(pc, 0), vpKey(pc, 1));
    // Same set (low bits untouched), distinct tags.
    LvpTable t(testConfig());
    t.train(vpKey(pc, 0), 111);
    t.train(vpKey(pc, 1), 222);
    EXPECT_EQ(t.lookup(vpKey(pc, 0)).value, 111u);
    EXPECT_EQ(t.lookup(vpKey(pc, 1)).value, 222u);
}

TEST(VpKey, PlainPcIsIdentity)
{
    EXPECT_EQ(vpKey(0x400890, 0), 0x400890u);
}
