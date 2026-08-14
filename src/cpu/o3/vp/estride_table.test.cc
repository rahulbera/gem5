#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

#include "cpu/o3/vp/estride_table.hh"
#include "cpu/o3/vp/eves_arbiter.hh"

using namespace gem5;
using namespace gem5::o3;

namespace
{

/** Scripted RNG: pops queued values; counts every draw. 0.0 always
 *  passes any bernoulli; 0.999... fails any exponent >= 1 (an
 *  exponent-0 draw passes regardless -- p = 1). */
struct ScriptedRng
{
    std::deque<double> values;
    unsigned draws = 0;

    double
    operator()()
    {
        draws++;
        if (values.empty()) {
            return 0.0;
        }
        double v = values.front();
        values.pop_front();
        return v;
    }
};

/** A classifier for an LLC-missing load (every latency term false):
 *  exponent 0 -- confidence and allocation draws pass at p = 1. */
EStrideClassifier
llcMissLoad()
{
    EStrideClassifier c;
    c.isLoad = true;
    c.notLlcMiss = false;
    c.notL2Miss = false;
    c.notL1Miss = false;
    c.fastInst = false;
    c.allocClass = EStrideAllocClass::Load;
    return c;
}

/** Drive key from freshly-allocated to a confident strided entry:
 *  miss-allocate at v0, then occurrences v0+s, v0+2s, ... until
 *  peek(key).conf >= EStrideTable::ConfThreshold. */
void
warmToConfident(EStrideTable &t, uint64_t key, uint64_t v0, int64_t s)
{
    const EStrideClassifier c = llcMissLoad();
    uint64_t v = v0;
    t.train(key, v, c); // allocate
    v += s;
    t.train(key, v, c); // first occurrence: stride set
    while (t.peek(key).conf < EStrideTable::ConfThreshold) {
        v += s;
        t.train(key, v, c);
    }
}

} // anonymous namespace

TEST(EStrideTable, VirginTableTagZeroHitsAndTrainsWithoutAllocating)
{
    // Spec S3.1: zero-init, no valid bit. Find a key whose way-0 tag
    // is 0; its first train() must go down the HIT path (first-
    // occurrence arm on the virgin entry), never the allocation path.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0;
    while (EStrideTable::wayTag(key, 0) != 0) {
        key += 64;
    }
    auto out = t.train(key, 700, llcMissLoad());
    ASSERT_EQ(out.size(), 1u);
    // delta = 700 - 0 in range and nonzero -> StrideSet, not any
    // Allocated* outcome.
    EXPECT_EQ(out[0], EStrideTrainOutcome::StrideSet);
    EXPECT_EQ(t.peek(key).stride, 700u);
}

TEST(EStrideTable, SafeStrideStartsAtZeroAndGatePassesFromCycleOne)
{
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    EXPECT_EQ(t.safeStride(), 0);
    uint64_t key = 0x1000;
    warmToConfident(t, key, 100, 8);
    EXPECT_TRUE(t.lookup(key, 0).predicted);
}

TEST(EStrideTable, PredictionExtrapolatesByInflightPlusOne)
{
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0x2000;
    warmToConfident(t, key, 1000, 24);
    const uint64_t last = t.peek(key).lastValue;
    EXPECT_EQ(t.lookup(key, 0).value, last + 24);
    EXPECT_EQ(t.lookup(key, 3).value, last + 4 * 24);
}

TEST(EStrideTable, NegativeStrideExtrapolatesSigned)
{
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0x3000;
    warmToConfident(t, key, 100000, -16);
    const uint64_t last = t.peek(key).lastValue;
    EXPECT_EQ(t.lookup(key, 2).value, last - 3 * 16);
}

TEST(EStrideTable, ConfSixDoesNotPredictConfSevenDoes)
{
    // Threshold endpoint (spec S3.2: conf >= 7 of 31).
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0x4000;
    const EStrideClassifier c = llcMissLoad();
    uint64_t v = 500;
    t.train(key, v, c);
    v += 8;
    t.train(key, v, c);
    while (t.peek(key).conf < EStrideTable::ConfThreshold - 1) {
        v += 8;
        t.train(key, v, c);
    }
    EXPECT_EQ(t.peek(key).conf, EStrideTable::ConfThreshold - 1);
    EXPECT_FALSE(t.lookup(key, 0).predicted);
    v += 8;
    t.train(key, v, c);
    EXPECT_EQ(t.peek(key).conf, EStrideTable::ConfThreshold);
    EXPECT_TRUE(t.lookup(key, 0).predicted);
}

TEST(EStrideTable, SafeStrideBlocksBeforeConfidence)
{
    // Gate order (spec S3.2): a saturating-confidence entry is
    // blocked while SafeStride < 0, and blockedBySafeStride reports
    // exactly the would-have-predicted case.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0x5000;
    warmToConfident(t, key, 100, 8);
    // warmToConfident() itself issues several train() calls, each of
    // which ticks SafeStride +1 unconditionally (S3.5) -- capture the
    // pre-penalty value rather than assume it is still 0.
    const int before = t.safeStride();
    ASSERT_GE(before, 0);
    EXPECT_TRUE(t.safeStridePenalty()); // crossed >= 0 to < 0
    EXPECT_EQ(t.safeStride(), before - 1024);
    const EStrideLookup l = t.lookup(key, 0);
    EXPECT_TRUE(l.hit);
    EXPECT_FALSE(l.predicted);
    EXPECT_TRUE(l.blockedBySafeStride);
}

TEST(EStrideTable, SafeStrideTickCreditPenaltyArithmetic)
{
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0x6000;
    // Tick: +1 per train call.
    t.train(key, 1, llcMissLoad());
    EXPECT_EQ(t.safeStride(), 1);
    // Credit: deliveredCorrect && stridePredicted, load -> +8 on top
    // of the tick.
    EStrideClassifier c = llcMissLoad();
    c.deliveredCorrect = true;
    c.stridePredicted = true;
    auto out = t.train(key, 9, c);
    EXPECT_NE(std::find(out.begin(), out.end(),
                        EStrideTrainOutcome::SafeStrideCredited),
              out.end());
    EXPECT_EQ(t.safeStride(), 1 + 1 + 8);
    // Non-load credit is +4.
    // Penalty: -1024, no lower clamp; stacking goes far below zero.
    EXPECT_TRUE(t.safeStridePenalty());
    EXPECT_FALSE(t.safeStridePenalty()); // already negative: no cross
    EXPECT_EQ(t.safeStride(), 10 - 2048);
}

TEST(EStrideTable, SafeStrideCapIsPreCheckedAndCanOvershoot)
{
    // Spec S3.5: the 32767 cap is a PRE-check -- from 32766 a load
    // credit legally lands the counter at 32774; further ticks and
    // credits are then refused.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    t.setSafeStrideForTest(32765);
    uint64_t key = 0x7000;
    EStrideClassifier c = llcMissLoad();
    c.deliveredCorrect = true;
    c.stridePredicted = true;
    t.train(key, 1, c); // tick 32765->32766 (< cap), credit +8
    EXPECT_EQ(t.safeStride(), 32774);
    t.train(key, 2, c); // tick refused, credit refused (>= cap)
    EXPECT_EQ(t.safeStride(), 32774);
}

TEST(EStrideTable, MispredictDecaysByFourAndConfFourCollapses)
{
    // Spec S3.3: conf > 4 -> conf -= 4 with u untouched; conf == 4
    // -> {0, 0}. Drive an entry to conf 8 / u 3, break the sequence.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0x8000;
    const EStrideClassifier c = llcMissLoad();
    warmToConfident(t, key, 100, 8); // conf >= 7 -> u jammed to 3
    while (t.peek(key).conf != 8) {
        uint64_t v = t.peek(key).lastValue;
        t.train(key, v + 8, c);
        ASSERT_LE(t.peek(key).conf, 8u);
    }
    auto out = t.train(key, t.peek(key).lastValue + 999, c);
    EXPECT_NE(std::find(out.begin(), out.end(),
                        EStrideTrainOutcome::MispredictDecay),
              out.end());
    EXPECT_EQ(t.peek(key).conf, 4u);
    EXPECT_EQ(t.peek(key).u, 3);           // u survives the decay
    EXPECT_FALSE(t.peek(key).notFirstOcc); // stride learning restarts
    // Re-establish the (same) stride, then break again at conf 4+...
    t.train(key, t.peek(key).lastValue + 8, c); // first-occ: StrideSet
    auto out2 = t.train(key, t.peek(key).lastValue + 999, c);
    // conf was 4 (== ConfDecay): the collapse arm fires.
    EXPECT_NE(std::find(out2.begin(), out2.end(),
                        EStrideTrainOutcome::MispredictCollapse),
              out2.end());
    EXPECT_EQ(t.peek(key).conf, 0u);
    EXPECT_EQ(t.peek(key).u, 0);
}

TEST(EStrideTable, ConstantValueSentinelDemotionLoop)
{
    // Spec S3.3: the 2-cycle sentinel loop for constant values --
    // conf and u pinned at 0, stride = 0xffff, notFirstOcc
    // oscillates; the entry stays a victim candidate forever.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0x9000;
    const EStrideClassifier c = llcMissLoad();
    t.train(key, 42, c); // allocate (conf = 1)
    for (int i = 0; i < 6; i++) {
        auto out = t.train(key, 42, c);
        const auto &want = (i % 2 == 0)
                               ? EStrideTrainOutcome::SentinelDemoted
                               : EStrideTrainOutcome::MispredictCollapse;
        EXPECT_NE(std::find(out.begin(), out.end(), want), out.end())
            << "iteration " << i;
    }
    EXPECT_EQ(t.peek(key).conf, 0u);
    EXPECT_EQ(t.peek(key).u, 0);
    EXPECT_EQ(t.peek(key).stride, EStrideTable::StrideSentinel);
}

TEST(EStrideTable, RangeWindowAsymmetricEndpoints)
{
    // Spec S3.3 / deviation 8: delta in [-2^19 + 1, +2^19]. +2^19 is
    // accepted as a stride; -2^19 is rejected (sentinel path).
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    const EStrideClassifier c = llcMissLoad();
    const int64_t kPos = int64_t{1} << 19;

    uint64_t key = 0xA000;
    t.train(key, 1000, c);
    auto out = t.train(key, 1000 + kPos, c);
    EXPECT_NE(
        std::find(out.begin(), out.end(), EStrideTrainOutcome::StrideSet),
        out.end());
    EXPECT_EQ(t.peek(key).stride, static_cast<uint64_t>(kPos));

    uint64_t key2 = 0xB000;
    t.train(key2, 10000000, c);
    auto out2 = t.train(key2, 10000000 - kPos, c);
    EXPECT_NE(std::find(out2.begin(), out2.end(),
                        EStrideTrainOutcome::SentinelDemoted),
              out2.end());
}

TEST(EStrideTable, RangeWindowHugeDeltaRejectedOverflowFree)
{
    // Spec S3.3: near-2^63 deltas must reject cleanly (the source's
    // raw abs(2*delta - 1) is UB here; our interval test is not).
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    const EStrideClassifier c = llcMissLoad();
    uint64_t key = 0xC000;
    t.train(key, 0, c);
    auto out = t.train(key, 0x8000000000000000ull, c);
    EXPECT_NE(std::find(out.begin(), out.end(),
                        EStrideTrainOutcome::SentinelDemoted),
              out.end());
}

TEST(EStrideTable, OutOfRangeDeltaOnTrainedEntryTakesMismatchArm)
{
    // Spec S10: a trained entry hit with an out-of-range delta routes
    // down the one-step-mismatch arm (raw identity check), never the
    // sentinel arm.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0xD000;
    warmToConfident(t, key, 100, 8);
    const unsigned conf_before = t.peek(key).conf;
    auto out = t.train(key, t.peek(key).lastValue + (uint64_t{1} << 40),
                       llcMissLoad());
    EXPECT_NE(std::find(out.begin(), out.end(),
                        EStrideTrainOutcome::MispredictDecay),
              out.end());
    EXPECT_EQ(t.peek(key).conf, conf_before - 4);
}

TEST(EStrideTable, DrawCountsByStrideRegimeAndSignedExclusion)
{
    // Spec S3.4: 1/2/4 draws by SIGNED stride regime; zero draws at
    // saturation; the deterministic conjunct consumes no draws.
    // Exponent > 0 so a draw is actually consumed: use an L1-hit load
    // (notLlc/notL2/notL1 true, fastInst false -> E = 3).
    EStrideClassifier c;
    c.isLoad = true;
    c.allocClass = EStrideAllocClass::Load;
    // (notLlcMiss/notL2Miss/notL1Miss default true, fastInst false)

    auto drawsForStride = [&](int64_t stride_val, uint64_t key) {
        ScriptedRng rng;
        EStrideTable t([&rng] { return rng(); });
        const EStrideClassifier alloc = llcMissLoad();
        t.train(key, 1 << 20, alloc);                // no draw (miss+p=1
                                                     // alloc: 1 draw at
                                                     // exponent 0 + 1
                                                     // way draw)
        t.train(key, (1 << 20) + stride_val, alloc); // StrideSet
        const unsigned before = rng.draws;
        // All queued fails: every conf draw AND u draw consumed.
        rng.values.assign(20, 0.999999);
        t.train(key, (1 << 20) + 2 * stride_val, c); // one-step match
        return rng.draws - before;
    };
    // conf: k draws (all fail) + u: k draws (all fail); no filter
    // coin (|stride| > 1).
    EXPECT_EQ(drawsForStride(4, 0xE000), 2u * 1u);
    EXPECT_EQ(drawsForStride(8, 0xE100), 2u * 2u);
    EXPECT_EQ(drawsForStride(64, 0xE200), 2u * 4u);
    EXPECT_EQ(drawsForStride(-64, 0xE300), 2u * 1u); // negative: k=1
}

TEST(EStrideTable, SaturatedCountersConsumeZeroDraws)
{
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0xE400;
    const EStrideClassifier c = llcMissLoad();
    warmToConfident(t, key, 0, 8);
    while (t.peek(key).conf < EStrideTable::ConfMax) {
        t.train(key, t.peek(key).lastValue + 8, c);
    }
    const unsigned before = rng.draws;
    t.train(key, t.peek(key).lastValue + 8, c);
    // conf saturated (31) and u jammed (3): zero increment draws.
    EXPECT_EQ(rng.draws, before);
}

TEST(EStrideTable, DeliveredCorrectWithoutStrideFlagBlocksWarming)
{
    // The deterministic conjunct (spec S3.4): VTAGE-covered correct
    // commits never warm the stride entry -- and consume no draws.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0xE500;
    EStrideClassifier c = llcMissLoad();
    t.train(key, 0, c);
    t.train(key, 8, c);
    const unsigned conf_before = t.peek(key).conf;
    const unsigned before = rng.draws;
    c.deliveredCorrect = true; // stridePredicted stays false
    t.train(key, 16, c);
    EXPECT_EQ(t.peek(key).conf, conf_before);
    EXPECT_EQ(rng.draws, before);
}

TEST(EStrideTable, UnitStrideLoadThrottles)
{
    // stride +1 loads: conf draw passes, then a 1/4 coin decides; the
    // coin is one extra draw. stride 0 never passes (covered by the
    // sentinel tests -- a zero stride never trains the match arm).
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    uint64_t key = 0xE600;
    const EStrideClassifier c = llcMissLoad(); // exponent 0: p = 1
    t.train(key, 100, c);
    t.train(key, 101, c); // stride +1 set
    // conf draw auto-passes (E=0 -> one consumed draw at p=1); coin
    // 0.999 fails -> ConfHeld. Same for u. 4 draws total.
    rng.values.assign({0.0, 0.999, 0.0, 0.999});
    const unsigned before = rng.draws;
    auto out = t.train(key, 102, c);
    EXPECT_EQ(rng.draws - before, 4u);
    EXPECT_NE(std::find(out.begin(), out.end(), EStrideTrainOutcome::ConfHeld),
              out.end());
    // Coin passes (< 0.25): conf increments. NOTE the entry was
    // installed at conf = 1 (the allocation seed), so the increment
    // lands it at 2.
    rng.values.assign({0.0, 0.2, 0.0, 0.999});
    t.train(key, 103, c);
    EXPECT_EQ(t.peek(key).conf, 2u);
}

TEST(EStrideTable, AllocationLadderPerClass)
{
    // AluOrStore 1/64, FpOrSlowAlu 1/16, Load by latency, Never no
    // draw at all. Verify via draw consumption + pass/fail edges.
    auto tryAlloc = [](EStrideAllocClass klass, bool is_load,
                       double first_draw, unsigned key) {
        ScriptedRng rng;
        EStrideTable t([&rng] { return rng(); });
        EStrideClassifier c;
        c.isLoad = is_load;
        c.allocClass = klass;
        // Load latency terms: L1-hit load (E' = 3) when is_load.
        rng.values.assign({first_draw, 0.0}); // alloc draw, way draw
        const unsigned before = rng.draws;
        auto out = t.train(key, 777, c);
        return std::make_pair(out, rng.draws - before);
    };
    // AluOrStore: p = 1/64. 0.9 fails; 0.01 passes.
    {
        auto [out, draws] =
            tryAlloc(EStrideAllocClass::AluOrStore, false, 0.9, 0xF000);
        EXPECT_EQ(out[0], EStrideTrainOutcome::AllocDrawRefused);
        EXPECT_EQ(draws, 1u);
    }
    {
        auto [out, draws] =
            tryAlloc(EStrideAllocClass::AluOrStore, false, 1.0 / 128, 0xF100);
        EXPECT_EQ(out[0], EStrideTrainOutcome::AllocatedConfZeroVictim);
        EXPECT_EQ(draws, 2u); // alloc draw + way draw
    }
    // Never: zero draws, no outcome... actually AllocDrawRefused is
    // pushed (the draw "fails" deterministically without consuming
    // rng) -- assert exactly that:
    {
        auto [out, draws] =
            tryAlloc(EStrideAllocClass::Never, false, 0.9, 0xF200);
        EXPECT_EQ(out[0], EStrideTrainOutcome::AllocDrawRefused);
        EXPECT_EQ(draws, 0u);
    }
}

TEST(EStrideTable, AllocSkippedWhenDeliveredCorrect)
{
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    EStrideClassifier c = llcMissLoad();
    c.deliveredCorrect = true;
    auto out = t.train(0xF300, 5, c);
    // Only the skip outcome (plus no SafeStride credit: the stride
    // flag is false).
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], EStrideTrainOutcome::AllocSkippedDeliveredCorrect);
}

TEST(EStrideTable, VictimPassOrderAndInstallState)
{
    // Fill all three ways of one set-group with confident entries,
    // then allocate a fourth key mapping there: pass 1 finds no
    // conf == 0, pass 2 finds no u == 0 (jammed to 3), so aging fires
    // on the last-probed way.
    //
    // wayIndex(key, way) always satisfies index % NumWays == way (an
    // invariant of the hash, cc:69-87: (X * NumWays + way) % NumEntries
    // with NumEntries a multiple of NumWays). So a single occupant
    // key's allocation -- which always installs at the physical slot
    // its OWN start-way draw picks -- can only ever land on ONE of
    // the three physical ways of a set-group. Three distinct occupant
    // keys are needed, one per way: for each way w, search for a key
    // whose OWN wayIndex(k, w) equals key0's slot for that way, and
    // force that key's allocation start-way draw (via the scripted
    // RNG) to w so its allocation actually lands there.
    ScriptedRng rng;
    EStrideTable t([&rng] { return rng(); });
    const uint64_t key0 = 0x10000;
    unsigned idx[3] = {EStrideTable::wayIndex(key0, 0),
                       EStrideTable::wayIndex(key0, 1),
                       EStrideTable::wayIndex(key0, 2)};
    std::vector<uint64_t> occupants;
    for (unsigned way = 0; way < 3; way++) {
        uint64_t k = 0x20000 + way * 0x1000;
        while (!(EStrideTable::wayIndex(k, way) == idx[way] &&
                 EStrideTable::wayTag(k, 0) != 0 &&
                 EStrideTable::wayTag(k, 1) != 0 &&
                 EStrideTable::wayTag(k, 2) != 0 &&
                 std::find(occupants.begin(), occupants.end(), k) ==
                     occupants.end())) {
            k += 8;
        }
        occupants.push_back(k);
        // Force this occupant's allocation start-way draw to `way`:
        // alloc draw (any value -- exponent 0 always passes for an
        // LLC-miss load), then the way draw landing in
        // [way/3, (way+1)/3).
        rng.values.push_back(0.0);
        rng.values.push_back((way + 0.5) / 3.0);
        warmToConfident(t, k, 100, 8); // conf >= 7, u = 3
    }
    // Precondition: key0 itself must MISS all three ways (no
    // accidental tag alias with an occupant's installed tag).
    ASSERT_FALSE(t.peek(key0).hit);
    // Aging draw: entry conf >= 7 -> exponent 2+2+2 = 6 (p = 1/64).
    // Script: alloc draw passes (exponent 0 for LLC-miss load), way
    // draw 0.0 -> start way 0, aging draw passes.
    rng.values.assign({0.0, 0.0, 0.0});
    auto out = t.train(key0, 999, llcMissLoad());
    EXPECT_NE(
        std::find(out.begin(), out.end(), EStrideTrainOutcome::AllocAged),
        out.end());
}

TEST(EStrideArbiter, FlagModeCrossProduct)
{
    // Spec S5/S10: pin payload selection, delivery, and the three
    // token bits over the full cross product.
    auto arb = [](bool sp, bool vh, bool vc, bool blackout,
                  bool require_conf) {
        EvesArbiterIn in;
        in.stridePredicted = sp;
        in.strideValue = 111;
        in.vtageHit = vh;
        in.vtageConfident = vc;
        in.vtageValue = 222;
        in.blackoutActive = blackout;
        in.overwriteRequiresConfidence = require_conf;
        return evesArbitrate(in);
    };
    // Verbatim mode: low-conf VTAGE hit CLOBBERS a confident stride.
    {
        auto o = arb(true, true, false, false, false);
        ASSERT_TRUE(o.value.has_value());
        EXPECT_EQ(*o.value, 222u);
        EXPECT_TRUE(o.stridePredicted);
        EXPECT_FALSE(o.vtageConfident);
        EXPECT_TRUE(o.deliveredByVtage);
    }
    // Ablation mode: the same case delivers the STRIDE value.
    {
        auto o = arb(true, true, false, false, true);
        ASSERT_TRUE(o.value.has_value());
        EXPECT_EQ(*o.value, 111u);
        EXPECT_FALSE(o.deliveredByVtage);
    }
    // Flag-less tag hit delivers NOTHING in either mode.
    {
        auto o = arb(false, true, false, false, false);
        EXPECT_FALSE(o.value.has_value());
        EXPECT_FALSE(o.deliveredByVtage);
    }
    // Both confident: VTAGE wins (either mode).
    {
        auto o = arb(true, true, true, false, false);
        EXPECT_EQ(*o.value, 222u);
        EXPECT_TRUE(o.deliveredByVtage);
        EXPECT_TRUE(o.stridePredicted);
        EXPECT_TRUE(o.vtageConfident);
    }
    // Blackout suppresses value AND flag: stride survives untouched.
    {
        auto o = arb(true, true, true, true, false);
        EXPECT_EQ(*o.value, 111u);
        EXPECT_FALSE(o.vtageConfident);
        EXPECT_FALSE(o.deliveredByVtage);
    }
    // VTAGE-only confident delivery.
    {
        auto o = arb(false, true, true, false, false);
        EXPECT_EQ(*o.value, 222u);
        EXPECT_TRUE(o.deliveredByVtage);
    }
    // Nothing anywhere.
    {
        auto o = arb(false, false, false, false, false);
        EXPECT_FALSE(o.value.has_value());
    }
}
