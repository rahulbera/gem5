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
    const MrnConfig cfg = testConfig();
    MrnTables tables(cfg);

    tables.commitStore(kAddr, kVal);
    // Binding has not happened yet: still a miss.
    EXPECT_FALSE(tables.predict(kPc).valid);

    for (unsigned i = 0; i < cfg.confThreshold; ++i) {
        tables.commitLoad(kPc, kAddr, kVal, /*isSpGp=*/false);
    }

    const MrnPrediction pred = tables.predict(kPc);
    EXPECT_TRUE(pred.valid);
    EXPECT_EQ(pred.value, kVal);
}

// 3. Once confident, a commit whose real value differs from the snapshot
//    resets the confidence and predict misses again.
TEST(MemRenamePredictor, ValueMismatchResetsConfidence)
{
    const MrnConfig cfg = testConfig();
    MrnTables tables(cfg);

    tables.commitStore(kAddr, kVal);
    for (unsigned i = 0; i < cfg.confThreshold; ++i) {
        tables.commitLoad(kPc, kAddr, kVal, /*isSpGp=*/false);
    }
    ASSERT_TRUE(tables.predict(kPc).valid);

    // Real value disagrees with the snapshot -> confidence reset.
    tables.commitLoad(kPc, kAddr, 0x9999, /*isSpGp=*/false);
    EXPECT_FALSE(tables.predict(kPc).valid);
}

// 4. An explicit mispredict (writeback path) clears confidence for loop
//    safety, so a previously confident load PC misses afterwards.
TEST(MemRenamePredictor, MispredictResetsConfidence)
{
    const MrnConfig cfg = testConfig();
    MrnTables tables(cfg);

    tables.commitStore(kAddr, kVal);
    for (unsigned i = 0; i < cfg.confThreshold; ++i) {
        tables.commitLoad(kPc, kAddr, kVal, /*isSpGp=*/false);
    }
    ASSERT_TRUE(tables.predict(kPc).valid);

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
    while (!slow.predict(kPc).valid) {
        slow.commitLoad(kPc, kAddr, kVal, /*isSpGp=*/false);
        ++slowCommits;
    }

    MrnTables fast(cfg);
    fast.commitStore(kAddr, kVal);
    unsigned fastCommits = 0;
    while (!fast.predict(kPc).valid) {
        fast.commitLoad(kPc, kAddr, kVal, /*isSpGp=*/true);
        ++fastCommits;
    }

    EXPECT_LT(fastCommits, slowCommits);
}
