#include <gtest/gtest.h>

#include "base/types.hh"
#include "cpu/o3/mem_rename_predictor.hh"

using gem5::Addr;
using gem5::RegVal;
using gem5::o3::MrnConfig;
using gem5::o3::MrnPrediction;
using gem5::o3::MrnTables;

namespace
{

// Small configuration so the tables are cheap to construct. confBits=4 gives
// a maximum confidence of 15, comfortably above the threshold of 8.
MrnConfig
testConfig()
{
    MrnConfig cfg;
    cfg.storeTableEntries = 64;
    cfg.storeTableAssoc = 4;
    cfg.loadTableEntries = 64;
    cfg.loadTableAssoc = 4;
    cfg.valueFileEntries = 32;
    cfg.confBits = 4;
    cfg.confThreshold = 8;
    cfg.confInc = 1;
    cfg.confDec = 1;
    cfg.resetConfOnMispredict = true;
    return cfg;
}

constexpr Addr kPc = 0x4000;
constexpr Addr kAddr = 0x00dead00;
constexpr RegVal kVal = 0x1234;

// Mirror one pipeline pass over a load under the default snapshot-training
// policy: snapshot the prediction at "rename" (peek, confidence-independent),
// then train against it at "commit". This is exactly what rename.cc /
// commit.cc do, so the core sees the same inputs it sees in a real run.
void
trainSnap(MrnTables &t, Addr pc, Addr addr, RegVal real, bool spgp = false)
{
    const MrnPrediction snap = t.peek(pc);
    t.commitLoad(pc, addr, real, spgp, /*train_on_snapshot=*/true, snap.valid,
                 snap.value);
}

// Train a stable value to a confident prediction; fails the test if it does
// not converge within a generous cap.
void
trainToConfident(MrnTables &t)
{
    for (unsigned i = 0; i < 64 && !t.predict(kPc).valid; ++i) {
        trainSnap(t, kPc, kAddr, kVal);
    }
    ASSERT_TRUE(t.predict(kPc).valid);
}

} // anonymous namespace

// 1. A cold predict (nothing trained) must miss.
TEST(MemRenamePredictor, ColdPredictInvalid)
{
    MrnTables tables(testConfig());
    EXPECT_FALSE(tables.predict(kPc).valid);
}

// 2. A store deposit plus enough matching load commits trains a load PC to
//    high confidence, after which predict returns the snapshotted value.
TEST(MemRenamePredictor, TrainsToHighConfidenceAndPredicts)
{
    MrnTables tables(testConfig());

    tables.commitStore(kAddr, kVal);
    // Binding has not happened yet: still a miss.
    EXPECT_FALSE(tables.predict(kPc).valid);

    trainToConfident(tables);
    EXPECT_EQ(tables.predict(kPc).value, kVal);
}

// 3. Once confident, a commit whose real value differs from the rename
//    snapshot resets the confidence and predict misses again.
TEST(MemRenamePredictor, ValueMismatchResetsConfidence)
{
    MrnTables tables(testConfig());

    tables.commitStore(kAddr, kVal);
    trainToConfident(tables);

    // The load's snapshot is still kVal, but it committed a different value
    // -> the prediction would have been wrong -> confidence reset.
    trainSnap(tables, kPc, kAddr, 0x9999);
    EXPECT_FALSE(tables.predict(kPc).valid);
}

// 4. An explicit mispredict (writeback path) clears confidence for loop
//    safety, so a previously confident load PC misses afterwards.
TEST(MemRenamePredictor, MispredictResetsConfidence)
{
    MrnTables tables(testConfig());

    tables.commitStore(kAddr, kVal);
    trainToConfident(tables);

    tables.mispredict(kPc);
    EXPECT_FALSE(tables.predict(kPc).valid);
}

// 5. The SpGp hint (2*confInc per train) reaches a confident prediction in
//    strictly fewer commits than the non-SpGp path.
TEST(MemRenamePredictor, SpGpRampsFaster)
{
    const MrnConfig cfg = testConfig();

    MrnTables slow(cfg);
    slow.commitStore(kAddr, kVal);
    unsigned slowCommits = 0;
    while (!slow.predict(kPc).valid && slowCommits < 64) {
        trainSnap(slow, kPc, kAddr, kVal, /*spgp=*/false);
        ++slowCommits;
    }

    MrnTables fast(cfg);
    fast.commitStore(kAddr, kVal);
    unsigned fastCommits = 0;
    while (!fast.predict(kPc).valid && fastCommits < 64) {
        trainSnap(fast, kPc, kAddr, kVal, /*spgp=*/true);
        ++fastCommits;
    }

    ASSERT_TRUE(slow.predict(kPc).valid);
    ASSERT_TRUE(fast.predict(kPc).valid);
    EXPECT_LT(fastCommits, slowCommits);
}

// 6. The 1a fix: a changing store->load recurrence must NOT build confidence
//    under snapshot training. Each iteration the producing store refreshes
//    the value file before the load commits, so the load's rename snapshot is
//    the PREVIOUS iteration's value and never matches what it reads.
TEST(MemRenamePredictor, ChangingRecurrenceStaysUnconfidentUnderSnapshot)
{
    const MrnConfig cfg = testConfig();
    MrnTables tables(cfg);

    RegVal v = 0x1000;
    for (unsigned i = 0; i < 4 * cfg.confThreshold; ++i) {
        // Snapshot at the load's rename: the slot still holds v_{i-1}.
        const MrnPrediction snap = tables.peek(kPc);
        // This iteration's store refreshes the slot to v_i before the load
        // commits.
        v += 1;
        tables.commitStore(kAddr, v);
        // The load reads v_i; its snapshot was v_{i-1} -> mismatch.
        tables.commitLoad(kPc, kAddr, v, /*isSpGp=*/false,
                          /*train_on_snapshot=*/true, snap.valid, snap.value);
    }
    EXPECT_FALSE(tables.predict(kPc).valid);
}

// 7. The same changing recurrence DOES build (false) confidence under the
//    legacy commit-time comparison -- exactly the bug 1a fixes. Here training
//    compares the value file AS OF COMMIT (already refreshed to v_i) against
//    the committed value v_i, so it spuriously matches every iteration.
TEST(MemRenamePredictor, ChangingRecurrenceBuildsFalseConfidenceLegacy)
{
    const MrnConfig cfg = testConfig();
    MrnTables tables(cfg);

    RegVal v = 0x1000;
    for (unsigned i = 0; i < 4 * cfg.confThreshold; ++i) {
        v += 1;
        tables.commitStore(kAddr, v);
        tables.commitLoad(kPc, kAddr, v, /*isSpGp=*/false,
                          /*train_on_snapshot=*/false);
    }
    EXPECT_TRUE(tables.predict(kPc).valid);
}
