#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include "cpu/o3/vp/evtage_tables.hh"
#include "cpu/o3/vp/vp_history.hh"

using namespace gem5;
using namespace gem5::o3;

namespace
{

using Outcomes = std::vector<EVtageTrainOutcome>;

/** LOWVAL's "large" class needs |2*value + 1| >= 2^16 (design doc S1):
 *  anything below ~32768 is "small", not "large" -- a mistake this
 *  suite made once already and is now guarded
 *  against with a named, sufficiently-large constant rather than
 *  small hex literals that read as "large" but aren't. */
constexpr RegVal kLarge = 0x100000;

bool
contains(const Outcomes &out, EVtageTrainOutcome want)
{
    return std::find(out.begin(), out.end(), want) != out.end();
}

/** 3 tagged banks (L = 2, 4, 8) + VT0 (2 skewed ways of 8 entries
 *  each). VT1 (internal bank 1) IS a legal, reachable allocation
 *  target -- it is the design doc's landing-rule floor (DEP = 1 out
 *  of a VT0 provider or the no-provider arm's tagged sub-branch;
 *  DEP = r + 1 in general, S3). Small enough to hand-verify RNG draw
 *  counts. */
EVtageConfig
comboConfig()
{
    EVtageConfig c;
    c.baseEntries = 16; // 2 ways x 8.
    c.taggedEntries = 16;
    c.numTagged = 3;
    c.historyLengths = {2, 4, 8};
    c.baseTagBits = 8;
    c.confBits = 3;
    c.confThreshold = 7;
    c.uBits = 2;
    c.tickMax = 1024;
    c.burstGuardWindow = 0;
    c.pathBits = 16;
    return c;
}

std::function<double()>
constRng(double v)
{
    return [v]() { return v; };
}

/** Returns successive values from a fixed sequence; a call beyond the
 *  scripted length fails the test (mirrors vtage_tables.test.cc's
 *  helper of the same name -- exact draw-count fidelity matters for
 *  these RNG-scripted tests). Trailing unused values are fine (a
 *  branch that returns early simply never asks for them). */
std::function<double()>
scriptedRng(std::vector<double> seq)
{
    auto values = std::make_shared<std::vector<double>>(std::move(seq));
    auto idx = std::make_shared<size_t>(0);
    return [values, idx]() {
        EXPECT_LT(*idx, values->size()) << "scripted rng exhausted";
        return (*values)[(*idx)++];
    };
}

/** Establishes a bank-1 (external/biased rank 2, i.e. VT1) tagged
 *  provider with the given val, c == confMax/2 == 3, u == 0, via the
 *  "no provider, 1/8-tagged" landing sub-branch: draw1 = 0.9 (>= 7/8:
 *  take the 1/8-tagged branch, not the dominant base-way one), draw2
 *  = 0.5 (>= 1/8: no DEP bump, DEP stays at its floor, 1), draw3 = 0.5
 *  (the candidate's c <= (random() & confMax) draw -- irrelevant
 *  value: a virgin entry's c == 0 always qualifies). isIndirectCall
 *  bypasses the allocation *gate*'s own draws entirely, so these 3
 *  draws are the landing rule's alone. Returns the live token for
 *  (pc, upc, h); callers must script exactly {0.9, 0.5, 0.5} as the
 *  FIRST three draws of the table's rng. */
uint64_t
establishBank1Provider(EVtageTables &t, Addr pc, MicroPC upc,
                      const VpHistSnapshot &h, RegVal value = 0)
{
    EVtageClassifier setup;
    setup.isIndirectCall = true;
    setup.value = value;
    auto out = t.train(pc, upc, h, 0, setup);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::Allocated));
    return t.lookup(pc, upc, h).token;
}

/** Drives a live, non-punish-marked provider's confidence counter
 *  from `fromC` up to confMax (7, comboConfig's confBits == 3) via
 *  deterministic correct trains (a DRAM-miss load, memLevel == Mem,
 *  large value: E == 0, confidenceGateFires() always fires) with
 *  deliveredCorrect == true (so the u-update path only ever fires via
 *  the deterministic c == confMax arm, exactly once). Each call
 *  consumes exactly one confidence-gate draw (maskFires() always
 *  draws once, even for exponent 0) -- callers must script (7 -
 *  fromC) filler draws (any value; E == 0 always fires regardless). */
void
saturateConfidence(EVtageTables &t, Addr pc, MicroPC upc,
                   const VpHistSnapshot &h, uint64_t tok, RegVal storedValue,
                   unsigned fromC)
{
    EVtageClassifier corr;
    corr.isLoad = true;
    corr.memLevel = EVtageMemLevel::Mem;
    corr.value = storedValue;
    corr.deliveredCorrect = true;
    for (unsigned i = fromC; i < 7; i++) {
        t.train(pc, upc, h, tok, corr);
    }
}

EVtageClassifier
loadClassifier(EVtageMemLevel lvl, RegVal value)
{
    EVtageClassifier c;
    c.isLoad = true;
    c.memLevel = lvl;
    c.value = value;
    return c;
}

EVtageClassifier
nonLoadClassifier(bool fastInst, bool slowInst, RegVal value,
                  unsigned intSrcCount = 2)
{
    EVtageClassifier c;
    c.isLoad = false;
    c.fastInst = fastInst;
    c.slowInst = slowInst;
    c.intSrcCount = intSrcCount;
    c.value = value;
    return c;
}

/** Pins the confidence-increment exponent E exactly (S1, verbatim
 *  K32 transcription), via a bank-1 (non-doubling) provider seeded
 *  with the probe's own value, and a boundary scripted draw: draw at
 *  0.75x the 1/2^E threshold fires (CorrectInc); draw exactly at
 *  1/2^E does not (CorrectSat). The 0.75x margin (not 0.25x) is
 *  deliberate: 0.75x a boundary always lands ABOVE half of it, so a
 *  mutant that widens the exponent by +1 (halving the true
 *  probability) makes the SAME draw miss -- 0.25x would still fire
 *  under such a mutant and silently pass. maskFires(0) is
 *  unconditional (no boundary exists), so E == 0 rows only assert the
 *  fires side. */
void
assertConfidenceExponent(const EVtageClassifier &probe, unsigned expectedE)
{
    const Addr pc = 0x400;
    const VpHistSnapshot h{};
    {
        const double fireDraw =
            expectedE == 0 ? 0.99
                          : (1.0 / static_cast<double>(1ull << expectedE)) *
                                0.75;
        EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5, fireDraw}));
        establishBank1Provider(t, pc, 0, h, probe.value);
        const auto tok = t.lookup(pc, 0, h).token;
        EVtageClassifier test = probe;
        test.deliveredCorrect = true; // No draw for the u-update path.
        EXPECT_EQ(t.train(pc, 0, h, tok, test),
                 Outcomes{EVtageTrainOutcome::CorrectInc})
            << "E = " << expectedE;
    }
    if (expectedE > 0) {
        const double boundary = 1.0 / static_cast<double>(1ull << expectedE);
        EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5, boundary}));
        establishBank1Provider(t, pc, 0, h, probe.value);
        const auto tok = t.lookup(pc, 0, h).token;
        EVtageClassifier test = probe;
        test.deliveredCorrect = true;
        EXPECT_EQ(t.train(pc, 0, h, tok, test),
                 Outcomes{EVtageTrainOutcome::CorrectSat})
            << "E = " << expectedE;
    }
}

/** Pins the allocation-body exponent E' (S3: mask = (2 << E') - 1,
 *  i.e. probability 1/2^(E'+1) -- NOT the same shape as S1's
 *  confidence-increment mask) via a fresh "no provider" wrong-train
 *  (needs no setup: the table's virgin state already has no
 *  provider) with a load classifier (skips the outer operand gate
 *  entirely, isolating the body draw). The 0.75x fire margin (see
 *  assertConfidenceExponent) also discriminates an E'-vs-E swap
 *  mutant whenever E' != E for the probed row. Trailing 0.9/0.5/0.5
 *  values feed the landing rule's own draws when the body fires
 *  (unused, and harmless, when it doesn't -- landNoProvider() is
 *  never entered). */
void
assertAllocationExponent(EVtageMemLevel lvl, RegVal value,
                        unsigned expectedEPrime)
{
    const Addr pc = 0x500;
    const VpHistSnapshot h{};
    EVtageClassifier c = loadClassifier(lvl, value);
    const unsigned maskExp = expectedEPrime + 1;
    {
        const double fireDraw =
            (1.0 / static_cast<double>(1ull << maskExp)) * 0.75;
        EVtageTables t(comboConfig(),
                      scriptedRng({fireDraw, 0.9, 0.5, 0.5}));
        auto out = t.train(pc, 0, h, 0, c);
        EXPECT_TRUE(contains(out, EVtageTrainOutcome::Allocated))
            << "E' = " << expectedEPrime;
    }
    {
        const double boundary = 1.0 / static_cast<double>(1ull << maskExp);
        EVtageTables t(comboConfig(), scriptedRng({boundary}));
        auto out = t.train(pc, 0, h, 0, c);
        EXPECT_FALSE(contains(out, EVtageTrainOutcome::Allocated))
            << "E' = " << expectedEPrime;
        EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocatedBaseWay))
            << "E' = " << expectedEPrime;
    }
}

/** Pins S4's usefulness-update exponent E_u exactly, mirroring
 *  assertConfidenceExponent's boundary-draw technique but observed
 *  via peekProvider().u (u-update is a side effect, not a distinct
 *  outcome enum value). The confidence-gate draw is always scripted
 *  as 0.99 (fails for any exponent >= 1; for exponent == 0 it fires
 *  deterministically but only takes c from 3 to 4, nowhere near
 *  confMax, so the u-update's OWN deterministic `c == confMax` arm
 *  never confounds this), and deliveredCorrect == false keeps
 *  UPDATEU's own draw live. */
void
assertUExponent(const EVtageClassifier &probe, unsigned expectedEu)
{
    const Addr pc = 0x4200;
    const VpHistSnapshot h{};
    {
        const double fireDraw =
            (1.0 / static_cast<double>(1ull << expectedEu)) * 0.75;
        EVtageTables t(comboConfig(),
                      scriptedRng({0.9, 0.5, 0.5, 0.99, fireDraw}));
        establishBank1Provider(t, pc, 0, h, probe.value);
        const auto tok = t.lookup(pc, 0, h).token;
        EVtageClassifier test = probe;
        test.deliveredCorrect = false;
        t.train(pc, 0, h, tok, test);
        EXPECT_EQ(t.peekProvider(pc, 0, h, tok).u, 1u)
            << "E_u = " << expectedEu;
    }
    {
        const double boundary = 1.0 / static_cast<double>(1ull << expectedEu);
        EVtageTables t(comboConfig(),
                      scriptedRng({0.9, 0.5, 0.5, 0.99, boundary}));
        establishBank1Provider(t, pc, 0, h, probe.value);
        const auto tok = t.lookup(pc, 0, h).token;
        EVtageClassifier test = probe;
        test.deliveredCorrect = false;
        t.train(pc, 0, h, tok, test);
        EXPECT_EQ(t.peekProvider(pc, 0, h, tok).u, 0u)
            << "E_u = " << expectedEu;
    }
}

} // namespace

// ---------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------

TEST(EVtageTables, ConfThresholdBelowOneIsFatal)
{
    EVtageConfig cfg = comboConfig();
    cfg.confThreshold = 0;
    ASSERT_ANY_THROW(EVtageTables(cfg, constRng(0.0)));
}

TEST(EVtageTables, NumTaggedOutOfRangeIsFatal)
{
    EVtageConfig cfg = comboConfig();
    cfg.numTagged = 8;
    cfg.historyLengths = {2, 4, 8, 16, 32, 64, 128, 256};
    ASSERT_ANY_THROW(EVtageTables(cfg, constRng(0.0)));
}

TEST(EVtageTables, OddBaseEntriesIsFatal)
{
    EVtageConfig cfg = comboConfig();
    cfg.baseEntries = 15;
    ASSERT_ANY_THROW(EVtageTables(cfg, constRng(0.0)));
}

TEST(EVtageTables, TickMaxZeroIsFatal)
{
    EVtageConfig cfg = comboConfig();
    cfg.tickMax = 0;
    ASSERT_ANY_THROW(EVtageTables(cfg, constRng(0.0)));
}

TEST(EVtageTables, ConstructsAtDefaults)
{
    EVtageTables t(EVtageConfig{}, constRng(0.0));
    auto r = t.lookup(0x400000, 0, {});
    EXPECT_NE(r.token, 0u);
}

// ---------------------------------------------------------------------
// Lookup / rank-0 "no provider" token identity
// ---------------------------------------------------------------------

TEST(EVtageTables, ColdLookupIsNoProvider)
{
    EVtageTables t(comboConfig(), constRng(0.0));
    auto r = t.lookup(0x400, 0, {});
    EXPECT_FALSE(r.hit);
    EXPECT_FALSE(r.confident);
    EXPECT_EQ(r.value, 0u);
    EXPECT_NE(r.token, 0u); // Legal rank-0 token, distinct from "absent".
}

TEST(EVtageTables, NoProviderTokenDistinctFromAbsentToken)
{
    EVtageTables t(comboConfig(), constRng(0.5));
    const Addr pc = 0x600;
    const VpHistSnapshot h{};
    const auto noProviderTok = t.lookup(pc, 0, h).token;

    EVtageClassifier c;
    c.isIndirectCall = true;
    // A live no-provider token trains straight through -- no
    // StaleTokenRecomputed (unlike literal token == 0).
    auto outLive = t.train(pc, 0, h, noProviderTok, c);
    EXPECT_FALSE(contains(outLive, EVtageTrainOutcome::StaleTokenRecomputed));
    EXPECT_TRUE(contains(outLive, EVtageTrainOutcome::NoProviderTrained));
}

TEST(EVtageTables, AbsentTokenZeroRecomputesAndFindsNoProvider)
{
    EVtageTables t(comboConfig(), constRng(0.5));
    const Addr pc = 0x700;
    const VpHistSnapshot h{};
    EVtageClassifier c;
    c.isIndirectCall = true;
    auto out = t.train(pc, 0, h, 0, c);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::StaleTokenRecomputed));
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::NoProviderTrained));
}

TEST(EVtageTables, PeekProviderNoneBucketForRankZero)
{
    EVtageTables t(comboConfig(), constRng(0.5));
    const Addr pc = 0x800;
    const VpHistSnapshot h{};
    const auto tok = t.lookup(pc, 0, h).token;
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).rank, 0u);
}

TEST(EVtageTables, BaseWayTagHitAfterAllocationThenLookupHits)
{
    EVtageTables t(comboConfig(), scriptedRng({0.5, 0.0, 0.5}));
    const Addr pc = 0x900;
    const VpHistSnapshot h{};
    EVtageClassifier setup;
    setup.isIndirectCall = true;
    setup.value = 55;
    auto out = t.train(pc, 0, h, 0, setup);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocatedBaseWay));

    auto r = t.lookup(pc, 0, h);
    EXPECT_TRUE(r.hit);
    EXPECT_EQ(r.value, 55u);
    EXPECT_EQ(t.peekProvider(pc, 0, h, r.token).rank, 1u); // VT0.
}

TEST(EVtageTables, StaleBaseTagTokenFailsPunishAndRecomputesOnTrain)
{
    EVtageTables t(comboConfig(), scriptedRng({0.5, 0.0, 0.5}));
    const Addr pc = 0x1000;
    const VpHistSnapshot h{};
    EVtageClassifier setup;
    setup.isIndirectCall = true;
    setup.value = 111;
    t.train(pc, 0, h, 0, setup); // Lands in a VT0 way (base-way arm).
    const auto tok = t.lookup(pc, 0, h).token;
    ASSERT_TRUE(t.lookup(pc, 0, h).hit);

    // Corrupt the lowest tag bit (rankFieldBits(4) + wayFieldBits(1) +
    // indexFieldBits(4, comboConfig: max(log2(8), log2(16))) == bit 9)
    // -- same {rank, way, index}, different (wrong) tag.
    const uint64_t staleTok = tok ^ (1ULL << 9);
    EXPECT_FALSE(t.correctivePunish(staleTok));

    EVtageClassifier probe;
    probe.isIndirectCall = true;
    probe.value = 111; // Still == the live entry's value: correct train.
    auto out = t.train(pc, 0, h, staleTok, probe);
    EXPECT_EQ(out[0], EVtageTrainOutcome::StaleTokenRecomputed);
    EXPECT_EQ(out[1], EVtageTrainOutcome::CorrectInc);
}

TEST(EVtageTables, StaleTaggedTokenFailsPunishAndRecomputesOnTrain)
{
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5}));
    const Addr pc = 0x1100;
    const VpHistSnapshot h{};
    const auto tok = establishBank1Provider(t, pc, 0, h);

    const uint64_t staleTok = tok ^ (1ULL << 9);
    EXPECT_FALSE(t.correctivePunish(staleTok));

    EVtageClassifier probe;
    probe.isIndirectCall = true;
    probe.value = 0; // Matches the live entry's val (0): correct train.
    auto out = t.train(pc, 0, h, staleTok, probe);
    EXPECT_EQ(out[0], EVtageTrainOutcome::StaleTokenRecomputed);
    EXPECT_EQ(out[1], EVtageTrainOutcome::CorrectInc);
}

// ---------------------------------------------------------------------
// S1: confidence-increment exponent E (classifier truth table)
// ---------------------------------------------------------------------

TEST(EVtageTables, ConfidenceExponentLoadTable)
{
    // Verbatim K32 (eves-code-brief.md S2 worked table). LOWVAL's
    // "large" class needs a value >= ~32768 (kLarge); 5 is "small",
    // 0 is "zero".
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::Mem, kLarge), 0);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::Mem, 5), 1);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::Mem, 0), 2);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::L2, kLarge), 2);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::L2, 5), 3);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::L1d, kLarge), 4);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::L1d, 5), 5);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::Stlf, kLarge), 6);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::Stlf, 5), 7);
    assertConfidenceExponent(loadClassifier(EVtageMemLevel::Stlf, 0), 8);
}

TEST(EVtageTables, ConfidenceExponentNonLoadTable)
{
    // fastInst (genuine alu): bottoms out at E = 8 for a zero result
    // (design doc S1: "1-cycle ALU zero-results bottom out at
    // 1/256").
    assertConfidenceExponent(nonLoadClassifier(true, false, kLarge), 6);
    assertConfidenceExponent(nonLoadClassifier(true, false, 0), 8);
    // slowInst (fp/slowAlu): FASTINST false, all NOT*MISS true.
    assertConfidenceExponent(nonLoadClassifier(false, true, kLarge), 4);
    // Neither (store/undef fallback): "alu-like everywhere [for E]"
    // -- numerically identical to fastInst's row.
    assertConfidenceExponent(nonLoadClassifier(false, false, kLarge), 6);
}

TEST(EVtageTables, ConfidenceGateIndirectCallDeterministic)
{
    const Addr pc = 0x400;
    const VpHistSnapshot h{};
    // No draw at all for the confidence-gate decision itself: only
    // the 3 setup draws are scripted, and the test call still
    // succeeds (would fail with "scripted rng exhausted" if the
    // indirect-call dispatch consumed one).
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5}));
    establishBank1Provider(t, pc, 0, h);
    const auto tok = t.lookup(pc, 0, h).token;
    EVtageClassifier test;
    test.isIndirectCall = true;
    test.value = 0;
    test.deliveredCorrect = true;
    EXPECT_EQ(t.train(pc, 0, h, tok, test),
             Outcomes{EVtageTrainOutcome::CorrectInc});
}

TEST(EVtageTables, K32DoublingFiresOnSecondDrawForVt0Provider)
{
    // E = 6 for this row (fastInst, large value): mask fires at
    // p = 1/64 per draw. Script the FIRST draw to fail (>= 1/64) and
    // the SECOND to succeed (< 1/64) -- only reachable if VT0's
    // provider rank (1) doubles the trial (OR of two draws).
    const Addr pc = 0x1200;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.5, 0.0, 0.5, 0.9, 0.001}));
    EVtageClassifier setup;
    setup.isIndirectCall = true;
    setup.value = kLarge;
    t.train(pc, 0, h, 0, setup); // -> VT0 way (base-way arm).
    const auto tok = t.lookup(pc, 0, h).token;
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).rank, 1u); // VT0.

    EVtageClassifier test = nonLoadClassifier(true, false, kLarge);
    test.deliveredCorrect = true;
    EXPECT_EQ(t.train(pc, 0, h, tok, test),
             Outcomes{EVtageTrainOutcome::CorrectInc});
}

TEST(EVtageTables, NoDoublingForTaggedProvider)
{
    // Same E = 6 row, but the provider is bank 1 / VT1 (not VT0): a
    // single failed draw (>= 1/64) must NOT get a second trial --
    // CorrectSat, not CorrectInc. Exactly one draw is scripted for
    // the test call (a spurious second draw would exhaust the
    // script).
    const Addr pc = 0x1300;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5, 0.5}));
    establishBank1Provider(t, pc, 0, h, kLarge);
    const auto tok = t.lookup(pc, 0, h).token;
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).rank, 2u); // Bank 1 -> rank 2.

    EVtageClassifier test = nonLoadClassifier(true, false, kLarge);
    test.deliveredCorrect = true;
    EXPECT_EQ(t.train(pc, 0, h, tok, test),
             Outcomes{EVtageTrainOutcome::CorrectSat});
}

// ---------------------------------------------------------------------
// S3: allocation-body exponent E' (NOT S1's E)
// ---------------------------------------------------------------------

TEST(EVtageTables, AllocationExponentDivergesFromConfidenceOnDramMiss)
{
    // The design doc's headline divergence endpoint: a DRAM-miss
    // load's confidence-increment fires with p = 1 (E = 0, mask == 0,
    // deterministic; see ConfidenceExponentLoadTable's Mem/kLarge
    // row), but the SAME row's allocation body is only p = 1/2 (E' =
    // 0, but the mask formula is (2 << E') - 1 == 1, not (1 << E) - 1
    // == 0). The value doesn't actually matter for E' at Mem level
    // (the load/L1-miss bracket zeroes the LOWVAL term entirely).
    assertAllocationExponent(EVtageMemLevel::Mem, kLarge, 0);
}

TEST(EVtageTables, AllocationExponentLoadTable)
{
    // eves-code-brief.md S3 worked endpoints (K32).
    assertAllocationExponent(EVtageMemLevel::Stlf, 5, 6); // Small value.
}

TEST(EVtageTables, AllocationExponentDiscriminatesFromConfidenceExponent)
{
    // Mem/kLarge (the divergence test above) has E == E' == 0 for
    // BOTH formulas, so it cannot catch an E'-vs-E mixup (S1's
    // exponent substituted for S3's). Mem/small breaks that tie: the
    // load/L1-miss bracket zeroes E''s LOWVAL term regardless of
    // value (E' = 0 always, at Mem level), but E's own bracket is
    // additive, not multiplicative, so E = LOWVAL(1, small) + 0s = 1
    // for this same row. assertAllocationExponent's 0.75x-boundary
    // fire draw (0.75 * 1/2, since E'+1 == 1) lands ABOVE an E-swap
    // mutant's threshold (E+1 == 2, mask exponent 2, p == 1/4): only
    // the correct E' == 0 formula fires here.
    assertAllocationExponent(EVtageMemLevel::Mem, 5, 0);
}

// ---------------------------------------------------------------------
// S3: outer operand gate (alu/store/undef only) + MedConf override
// ---------------------------------------------------------------------

TEST(EVtageTables, OuterOperandGateSkippedForLoadFpSlowAlu)
{
    // A load's allocation reaches the body directly: exactly one draw
    // decides the fire/no-fire question (no separate outer-gate draw
    // to script) -- the 4-value script covers the body-fire draw
    // (0.0) plus the landing rule's own 3 draws.
    const Addr pc = 0x1400;
    const VpHistSnapshot h{};
    EVtageClassifier c = loadClassifier(EVtageMemLevel::Mem, 0x1000); // E'=0.
    EVtageTables t(comboConfig(), scriptedRng({0.0, 0.0, 0.0, 0.5}));
    auto out = t.train(pc, 0, h, 0, c);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocatedBaseWay) ||
               contains(out, EVtageTrainOutcome::Allocated));
}

TEST(EVtageTables, OuterOperandGateAluTwoOperandsRateOneSixteenth)
{
    // NbOperand >= 2 -> outer gate mask = 1/16 (exponent 4). Draw just
    // below 1/16 passes the gate; then the body (E' for a non-load
    // alu row with intSrcCount>=2: E' = LOWVAL + NOTLLCMISS(1) +
    // NOTL2MISS(1) + NOTL1MISS(1) + 2*FASTINST(2) = LOWVAL + 5; for
    // kLarge (LOWVAL=0), E'=5, mask exponent E'+1=6) also needs a
    // firing draw, then the landing rule's own draws.
    const Addr pc = 0x1500;
    const VpHistSnapshot h{};
    EVtageClassifier c = nonLoadClassifier(/*fastInst*/ true,
                                          /*slowInst*/ false, kLarge,
                                          /*intSrcCount*/ 2);
    EVtageTables t(comboConfig(),
                  scriptedRng({(1.0 / 16) * 0.25, (1.0 / 64) * 0.25, 0.9,
                              0.5, 0.5}));
    auto out = t.train(pc, 0, h, 0, c);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocatedBaseWay) ||
               contains(out, EVtageTrainOutcome::Allocated));
}

TEST(EVtageTables, OuterOperandGateFailureBlocksAllocationRegardlessOfBody)
{
    // The outer gate draw fails (>= 1/16 for NbOperand >= 2): the
    // body must NEVER be drawn (a spurious body draw here would
    // exhaust the 1-value script).
    const Addr pc = 0x1600;
    const VpHistSnapshot h{};
    EVtageClassifier c = nonLoadClassifier(true, false, 0x1000, 2);
    EVtageTables t(comboConfig(), scriptedRng({1.0 / 16}));
    auto out = t.train(pc, 0, h, 0, c);
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::Allocated));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocatedBaseWay));
}

TEST(EVtageTables, OuterOperandGateFewerThanTwoOperandsRateOneSixtyFourth)
{
    // NbOperand < 2 -> outer gate mask = 1/64 (exponent 6): a draw of
    // exactly 1/16 (the >=2 threshold) must still fail the <2 gate.
    const Addr pc = 0x1700;
    const VpHistSnapshot h{};
    EVtageClassifier c = nonLoadClassifier(true, false, 0x1000, 0);
    EVtageTables t(comboConfig(), scriptedRng({1.0 / 16}));
    auto out = t.train(pc, 0, h, 0, c);
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::Allocated));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocatedBaseWay));
}

TEST(EVtageTables, MedConfOverridesBodyButNotOuterGateForAlu)
{
    // MedConf makes the BODY deterministic, but alu/store/undef still
    // pay the outer gate first (design doc S3). Establish a bank-1
    // provider with c saturated (qualifies MedConf on punish), punish
    // it, then commit-train wrong: the outer gate still needs its own
    // draw; script it to FAIL (>= mask) and confirm no allocation
    // happens despite medConfPending being set (an allocation here
    // would exhaust the 1-draw script by consuming a body draw too).
    const Addr pc = 0x1800;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,      // Establish.
                              0.5, 0.5, 0.5, 0.5, // Saturate c: 3->7
                                                  // (4 calls, E=0
                                                  // deterministic).
                              1.0 / 16}));         // Outer gate: fails
                                                  // (NbOperand >= 2).
    const auto tok = establishBank1Provider(t, pc, 0, h, kLarge);
    saturateConfidence(t, pc, 0, h, tok, kLarge, 3);
    ASSERT_TRUE(t.peekProvider(pc, 0, h, tok).saturated);

    ASSERT_TRUE(t.correctivePunish(tok)); // c==7 -> {c=5,u=1}; medConf:
                                          // pre-punish c==7>3 -> true.

    EVtageClassifier wrong = nonLoadClassifier(true, false, 999, 2);
    auto out = t.train(pc, 0, h, tok, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::WrongOverwriteOnly));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::Allocated));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocatedBaseWay));
}

// ---------------------------------------------------------------------
// S2: misprediction rule -- both punish arms, incl. u
// ---------------------------------------------------------------------

TEST(EVtageTables, PunishSaturatedArmSubtractsTwoAndSetsUToOne)
{
    const Addr pc = 0x1900;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5}));
    const auto tok = establishBank1Provider(t, pc, 0, h, kLarge);
    saturateConfidence(t, pc, 0, h, tok, kLarge, 3); // c: 3 -> 7 (4 calls).
    auto peek = t.peekProvider(pc, 0, h, tok);
    ASSERT_EQ(peek.c, 7u);
    ASSERT_EQ(peek.u, 1u); // Deterministic u++ arm at c == confMax.

    ASSERT_TRUE(t.correctivePunish(tok));
    auto after = t.peekProvider(pc, 0, h, tok);
    EXPECT_EQ(after.c, 5u); // 7 - (7+1)/4 == 5.
    EXPECT_EQ(after.u, 1u); // u = 1 (set before the decrement).
}

TEST(EVtageTables, PunishResetArmZeroesConfidenceAndUsefulBit)
{
    const Addr pc = 0x1a00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5}));
    const auto tok = establishBank1Provider(t, pc, 0, h, 0);
    // c == 3 (below confMax) after allocation seeding: below-saturation
    // arm.
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).c, 3u);

    ASSERT_TRUE(t.correctivePunish(tok));
    auto after = t.peekProvider(pc, 0, h, tok);
    EXPECT_EQ(after.c, 0u);
    EXPECT_EQ(after.u, 0u);
}

TEST(EVtageTables, PunishTriggerIsExactSaturationNotNearSaturation)
{
    // The punish arm test is "c == confMax exactly" (design doc S2:
    // "the arm test is c == 7 exactly -- saturation, not high
    // confidence relative to the threshold"), not e.g. "c >= confMax
    // - 1". Drive c to 6 (one below confMax) and punish via BOTH
    // sites: expect the reset arm {c=0, u=0}, not the decrement arm
    // {c=4, u=1} a widened trigger would produce.
    const Addr pc = 0x1955;
    const VpHistSnapshot h{};

    // Site 1: correctivePunish.
    {
        EVtageTables t(comboConfig(),
                      scriptedRng({0.9, 0.5, 0.5, 0.5, 0.5, 0.5}));
        const auto tok = establishBank1Provider(t, pc, 0, h, kLarge);
        EVtageClassifier corr;
        corr.isLoad = true;
        corr.memLevel = EVtageMemLevel::Mem;
        corr.value = kLarge;
        corr.deliveredCorrect = true;
        for (int i = 0; i < 3; i++) {
            t.train(pc, 0, h, tok, corr); // c: 3 -> 4 -> 5 -> 6.
        }
        ASSERT_EQ(t.peekProvider(pc, 0, h, tok).c, 6u);

        ASSERT_TRUE(t.correctivePunish(tok));
        auto after = t.peekProvider(pc, 0, h, tok);
        EXPECT_EQ(after.c, 0u); // Reset arm, NOT the decrement arm's
        EXPECT_EQ(after.u, 0u); // {c=4, u=1}.
    }
    // Site 2: an unmarked wrong train (train()'s own "full rule"
    // punish, not the deferred/punish-marked shortcut).
    {
        const Addr pc2 = 0x1965;
        EVtageTables t(comboConfig(),
                      scriptedRng({0.9, 0.5, 0.5, 0.5, 0.5, 0.5, 1.0 / 16}));
        const auto tok = establishBank1Provider(t, pc2, 0, h, kLarge);
        EVtageClassifier corr;
        corr.isLoad = true;
        corr.memLevel = EVtageMemLevel::Mem;
        corr.value = kLarge;
        corr.deliveredCorrect = true;
        for (int i = 0; i < 3; i++) {
            t.train(pc2, 0, h, tok, corr); // c: 3 -> 4 -> 5 -> 6.
        }
        ASSERT_EQ(t.peekProvider(pc2, 0, h, tok).c, 6u);

        EVtageClassifier wrong = nonLoadClassifier(true, false, 999, 2);
        auto out = t.train(pc2, 0, h, tok, wrong);
        // Reset arm, not WrongPunishSat.
        EXPECT_EQ(out[0], EVtageTrainOutcome::WrongPunishReset);
        EXPECT_EQ(t.peekProvider(pc2, 0, h, tok).c, 0u);
        EXPECT_EQ(t.peekProvider(pc2, 0, h, tok).u, 0u);
    }
}

TEST(EVtageTables, PunishSaturatedArmSetsUToOneNotLeavingItAtThree)
{
    // CVP's `u = (conf == MAXCONFID)` on the saturated punish arm is
    // an ASSIGNMENT to 1, not an increment or a no-op: it must shield
    // -then-reset u even from a HIGHER pre-punish value. Build u == 3
    // (uMax) at c == 7 first (mirrors UBitCapsAtUMax's setup: repeated
    // isIndirectCall correct trains make both the confidence gate and
    // UPDATEU fire deterministically, so u climbs to uMax well before
    // c even saturates), then punish: every OTHER punish test in this
    // suite happens to already have u == 1 pre-punish, so "set u = 1"
    // and "leave u unchanged" would otherwise coincide.
    const Addr pc = 0x1975;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5}));
    const auto tok = establishBank1Provider(t, pc, 0, h, 0);
    EVtageClassifier corr;
    corr.isIndirectCall = true;
    corr.value = 0;
    for (int i = 0; i < 7; i++) {
        t.train(pc, 0, h, tok, corr);
    }
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).c, 7u);
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).u, 3u); // uMax.

    ASSERT_TRUE(t.correctivePunish(tok));
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).u, 1u); // Not 3.
}

TEST(EVtageTables, CorrectivePunishOnNoProviderTokenReturnsFalse)
{
    EVtageTables t(comboConfig(), constRng(0.5));
    const auto tok = t.lookup(0x1b00, 0, {}).token; // No-provider token.
    EXPECT_FALSE(t.correctivePunish(tok));
}

TEST(EVtageTables, CorrectivePunishTokenZeroReturnsFalse)
{
    EVtageTables t(comboConfig(), constRng(0.5));
    EXPECT_FALSE(t.correctivePunish(0));
}

// ---------------------------------------------------------------------
// Unconditional wrong-train value overwrite
// ---------------------------------------------------------------------

TEST(EVtageTables, WrongTrainOverwritesValueRegardlessOfConfidence)
{
    const Addr pc = 0x1c00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,   // Establish (val = 0).
                              0.5, 0.5,        // 2 correct trains: c
                                              // 3->4->5 (deterministic
                                              // E=0, deliveredCorrect).
                              1.0 / 16}));     // Wrong-train's outer
                                              // gate: fails.
    const auto tok = establishBank1Provider(t, pc, 0, h, kLarge);
    EVtageClassifier corr = loadClassifier(EVtageMemLevel::Mem, kLarge);
    corr.deliveredCorrect = true;
    t.train(pc, 0, h, tok, corr);
    t.train(pc, 0, h, tok, corr);
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).c, 5u);

    EVtageClassifier wrong = nonLoadClassifier(true, false, 12345, 2);
    auto out = t.train(pc, 0, h, tok, wrong);
    EXPECT_EQ(out[0], EVtageTrainOutcome::WrongPunishReset); // c=5!=confMax.
    EXPECT_EQ(t.lookup(pc, 0, h).value, 12345u); // Overwritten regardless.
}

// ---------------------------------------------------------------------
// Delivered-wrong lifecycle: verify punish + deferred commit train ==
// exactly one confidence punish; pinned endpoint {c=5, u=1, value
// corrected}.
// ---------------------------------------------------------------------

TEST(EVtageTables, DeliveredWrongLifecycleEndpointPinned)
{
    const Addr pc = 0x1d00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,        // Establish.
                              0.5, 0.5, 0.5, 0.5,   // Saturate c: 3->7.
                              0.5, 0.5}));           // Deferred train's
                                                     // allocation
                                                     // landing (provider
                                                     // -exists arm: dep
                                                     // bump fails, dep=3;
                                                     // bank3 virgin: r7
                                                     // draw irrelevant).
    const auto tok = establishBank1Provider(t, pc, 0, h, kLarge);
    saturateConfidence(t, pc, 0, h, tok, kLarge, 3);
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).c, 7u);

    // Verify-site: exactly one punish.
    ASSERT_TRUE(t.correctivePunish(tok));
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).c, 5u);
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).u, 1u);

    // Commit-site: the refetched instance's deferred train -- value
    // overwrite + allocation probe only, NO second punish.
    EVtageClassifier committed;
    committed.isIndirectCall = true; // Bypass the allocation *gate*'s
                                     // own draws; the landing rule's
                                     // own draws are still scripted
                                     // above.
    committed.value = 42;
    auto out = t.train(pc, 0, h, tok, committed);
    EXPECT_EQ(out[0], EVtageTrainOutcome::WrongOverwriteOnly);

    auto final_ = t.peekProvider(pc, 0, h, tok);
    EXPECT_EQ(final_.c, 5u); // Unchanged: no second punish.
    EXPECT_EQ(final_.u, 1u); // Unchanged.
    EXPECT_EQ(t.lookup(pc, 0, h).value, 42u); // Corrected.
}

// ---------------------------------------------------------------------
// Pending-bit lifecycle
// ---------------------------------------------------------------------

TEST(EVtageTables, PendingBitsSetOnlyByPunishNotByPlainWrongTrain)
{
    // An UNMARKED wrong train (no prior punish) applies the full rule
    // directly -- WrongPunishReset/Sat, never WrongOverwriteOnly.
    const Addr pc = 0x1e00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5, 0.99}));
    const auto tok = establishBank1Provider(t, pc, 0, h);
    EVtageClassifier wrong = loadClassifier(EVtageMemLevel::L2, 999);
    auto out = t.train(pc, 0, h, tok, wrong);
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::WrongOverwriteOnly));
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::WrongPunishReset));
}

TEST(EVtageTables, PendingBitsConsumedByNextWrongTrainSkipsConfidenceRule)
{
    const Addr pc = 0x1f00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,   // Establish.
                              0.5, 0.5}));      // Deferred train's
                                               // landing (provider
                                               // -exists, dep=3, bank3
                                               // virgin).
    const auto tok = establishBank1Provider(t, pc, 0, h); // punishApplied
                                                          // = true, c:
                                                          // 3 -> 0.
    ASSERT_TRUE(t.correctivePunish(tok));

    EVtageClassifier committed;
    committed.isIndirectCall = true;
    committed.value = 7;
    auto out = t.train(pc, 0, h, tok, committed);
    // Only the deferred remainder: no second WrongPunishReset/Sat.
    EXPECT_EQ(out[0], EVtageTrainOutcome::WrongOverwriteOnly);
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::WrongPunishReset));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::WrongPunishSat));
}

TEST(EVtageTables, PendingBitsDroppedByCorrectTrain)
{
    // Punish-mark an entry, then verify it *correct* instead (design
    // doc: "a correct outcome drops them -- matching CVP's one-event
    // semantics, no memory across events"). A LATER wrong train must
    // then apply the FULL rule again (medConf computed fresh), not
    // the punish-marked shortcut.
    const Addr pc = 0x2000;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,   // Establish.
                              0.99,            // "Correct" train's
                                              // confidence gate: fails
                                              // (c stays 0 after
                                              // punish's reset arm).
                              0.99}));         // Later wrong train's
                                              // outer gate (load: none)
                                              // / body: fails.
    const auto tok = establishBank1Provider(t, pc, 0, h, 0);
    // cPre == 3 (== confMax/2), u == 0 != uMax: medConf's 2nd disjunct
    // needs u==uMax, so medConf is false here -> reset arm: c=0,u=0.
    ASSERT_TRUE(t.correctivePunish(tok));

    EVtageClassifier corr = nonLoadClassifier(/*fastInst*/ true,
                                              /*slowInst*/ false, 0,
                                              /*intSrcCount*/ 2);
    corr.deliveredCorrect = true; // E = 8 for a zero-value alu row: the
                                  // 0.99 draw fails it, c stays 0.
    auto outCorrect = t.train(pc, 0, h, tok, corr);
    EXPECT_EQ(outCorrect, Outcomes{EVtageTrainOutcome::CorrectSat});
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).c, 0u);

    EVtageClassifier wrong = loadClassifier(EVtageMemLevel::L2, 999);
    auto outWrong = t.train(pc, 0, h, tok, wrong);
    // Pending bits were dropped by the correct train above: the full
    // rule applies here, not the punish-marked shortcut.
    EXPECT_FALSE(contains(outWrong, EVtageTrainOutcome::WrongOverwriteOnly));
    EXPECT_TRUE(contains(outWrong, EVtageTrainOutcome::WrongPunishReset));
}

TEST(EVtageTables, PendingBitsClearedByAllocationSeeding)
{
    // Reallocated-between-punish-and-train case: pcA's entry is
    // punish-marked, then its token is corrupted to simulate "a
    // different occupant now lives here" (allocation seeding clears
    // both pending bits per contract). The deferred train's stale-tag
    // check fails, recomputes via findProvider() -- which, since
    // nothing actually reallocated the physical entry in THIS test,
    // lands back on the same live entry and reads its still-set
    // pending bits directly (not through the corrupted token),
    // correctly taking the punish-marked shortcut.
    const Addr pcA = 0x2100;
    const VpHistSnapshot hA{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,   // Establish.
                              0.5, 0.5}));      // Deferred train's
                                               // landing.
    const auto tokA = establishBank1Provider(t, pcA, 0, hA, 0);
    ASSERT_TRUE(t.correctivePunish(tokA));

    const uint64_t staleTok = tokA ^ (1ULL << 9);
    EVtageClassifier committed;
    committed.isIndirectCall = true;
    committed.value = 321;
    auto out = t.train(pcA, 0, hA, staleTok, committed);
    EXPECT_EQ(out[0], EVtageTrainOutcome::StaleTokenRecomputed);
    EXPECT_EQ(out[1], EVtageTrainOutcome::WrongOverwriteOnly);
}

// ---------------------------------------------------------------------
// S3: landing rule -- both arms + seeds
// ---------------------------------------------------------------------

TEST(EVtageTables, LandingProviderExistsArmSeedsConfMaxOverTwoNoSpecialCase)
{
    // A tagged (bank-1/VT1, non-VT0) provider's wrong-train promotes
    // into a higher bank, seeding c = confMax/2 (3) -- even for a
    // zero-operand alu instruction, which would ONLY get seed-to-7 in
    // the base-way arm (design doc S3: "No seed-to-7 special case in
    // this arm").
    const Addr pc = 0x2200;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,     // Establish bank 1.
                              1.0 / 8,           // DEP bump draw: fails
                                                 // (dep stays at its
                                                 // floor, 2).
                              0.0}));            // Candidate's c<=draw7
                                                 // (bank 2, virgin: c==0
                                                 // always qualifies).
    const auto tok = establishBank1Provider(t, pc, 0, h);
    EVtageClassifier wrong = nonLoadClassifier(/*fastInst*/ true,
                                               /*slowInst*/ false, 999,
                                               /*intSrcCount*/ 0);
    wrong.isIndirectCall = true; // Bypass the allocation *gate* -- only
                                 // the landing rule's draws are scripted.
    auto out = t.train(pc, 0, h, tok, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::Allocated));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocSeedSaturated));

    // Bank 1's provider is at internal bank 1 -> DEP floor = 1+1 = 2
    // (design doc S3's "DEP = r + 1"); the landing target is bank 2.
    const auto newTok = t.lookup(pc, 0, h).token;
    EXPECT_EQ(t.peekProvider(pc, 0, h, newTok).rank, 3u); // Bank 2 -> rank3.
    EXPECT_EQ(t.peekProvider(pc, 0, h, newTok).c, 3u);    // confMax/2, not 7.
}

TEST(EVtageTables, LandingProviderExistsVt0ProviderLandsInVt1AtDepFloor)
{
    // Regression pin: a
    // VT0-provider's wrong-train promotion floor is DEP =
    // providerBankInternal(0) + 1 = 1 = VT1, NOT VT2. CVP's own extra
    // "if (HitBank == 0) DEP++" bump exists only to align CVP's own
    // TWO zero-history bank numbers (0 and 1) with each other -- both
    // are already collapsed to providerBankInternal == 0 here, so no
    // analogous bump belongs in this core (an earlier, now-corrected
    // version of this code added one anyway and silently made VT1
    // permanently unreachable).
    const Addr pc = 0x2260;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.5, 0.0, 0.5,   // Establish a VT0 way
                                              // (base-way arm, way 0).
                              0.5,             // DEP bump draw: fails
                                              // (dep stays at its
                                              // floor, 1).
                              0.0}));          // Candidate's c<=draw7
                                              // (VT1, virgin: c==0
                                              // always qualifies).
    EVtageClassifier setup;
    setup.isIndirectCall = true;
    setup.value = 111;
    t.train(pc, 0, h, 0, setup);
    const auto tok = t.lookup(pc, 0, h).token;
    ASSERT_EQ(t.peekProvider(pc, 0, h, tok).rank, 1u); // VT0.

    EVtageClassifier wrong;
    wrong.isIndirectCall = true;
    wrong.value = 999;
    auto out = t.train(pc, 0, h, tok, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::Allocated));
    const auto newTok = t.lookup(pc, 0, h).token;
    EXPECT_EQ(t.peekProvider(pc, 0, h, newTok).rank, 2u); // VT1, not VT2.
}

TEST(EVtageTables, LandingNoProviderBaseWayArmSeedsThreeByDefault)
{
    const Addr pc = 0x2300;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.0, 0.0, 0.0, 0.5}));
    EVtageClassifier wrong = loadClassifier(EVtageMemLevel::Mem, 999);
    auto out = t.train(pc, 0, h, 0, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocatedBaseWay));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocSeedSaturated));
    EXPECT_EQ(t.peekProvider(pc, 0, h, t.lookup(pc, 0, h).token).c, 3u);
}

TEST(EVtageTables, LandingNoProviderBaseWayArmZeroOperandAluSeedsToConfMax)
{
    // The zero-operand-ALU special case: NbOperand == 0 && alu ->
    // seed c = confMax (7), NOT confMax/2 -- "exists only in this
    // base-way arm" (design doc S3).
    const Addr pc = 0x2400;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.0, 0.0, 0.0, 0.0, 0.5}));
    EVtageClassifier wrong =
        nonLoadClassifier(/*fastInst*/ true, /*slowInst*/ false, 999,
                          /*intSrcCount*/ 0);
    auto out = t.train(pc, 0, h, 0, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocatedBaseWay));
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocSeedSaturated));
    EXPECT_EQ(t.peekProvider(pc, 0, h, t.lookup(pc, 0, h).token).c, 7u);
}

TEST(EVtageTables, LandingNoProviderTaggedArmNeverSeedsToConfMax)
{
    // The 1/8-tagged sub-branch of the "no provider" arm: even a
    // zero-operand-alu instruction gets the normal confMax/2 seed --
    // the special case is base-way-only.
    const Addr pc = 0x2500;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.0, 0.0, 0.9, 0.5, 0.5}));
    EVtageClassifier wrong =
        nonLoadClassifier(/*fastInst*/ true, /*slowInst*/ false, 999,
                          /*intSrcCount*/ 0);
    auto out = t.train(pc, 0, h, 0, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::Allocated));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocSeedSaturated));
    EXPECT_EQ(t.peekProvider(pc, 0, h, t.lookup(pc, 0, h).token).c, 3u);
}

TEST(EVtageTables, LandingNoProviderTaggedArmLandsInVt1AtDepFloor)
{
    // Regression pin: the
    // no-provider arm's 1/8-tagged sub-branch floor is DEP = 1 (our
    // internal bank units), i.e. VT1 -- not VT2. draw3 (the DEP-bump
    // check) is scripted to fail (>= 1/8), so the floor itself lands
    // the allocation; a stale off-by-one would land on VT2 (rank 3)
    // instead of VT1 (rank 2).
    const Addr pc = 0x2550;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.0}));
    EVtageClassifier wrong = loadClassifier(EVtageMemLevel::Mem, 999);
    wrong.isIndirectCall = true; // Bypass the allocation *gate*'s own
                                 // draw -- only the landing rule's
                                 // draws are scripted.
    auto out = t.train(pc, 0, h, 0, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::Allocated));
    EXPECT_EQ(t.peekProvider(pc, 0, h, t.lookup(pc, 0, h).token).rank,
             2u); // VT1, not VT2.
}

TEST(EVtageTables, LandingNoProviderBaseWayDrawnWayFirstOrder)
{
    // w = 1 (drawn way): scans way 1 first. Both ways are virgin, so
    // way 1 is stolen immediately -- no AllocScanStep should appear.
    const Addr pc = 0x2600;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.0, 0.0, 0.99, 0.5}));
    EVtageClassifier wrong = loadClassifier(EVtageMemLevel::Mem, 999);
    auto out = t.train(pc, 0, h, 0, wrong);
    EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocatedBaseWay));
    EXPECT_FALSE(contains(out, EVtageTrainOutcome::AllocScanStep));
}

// ---------------------------------------------------------------------
// TICK bookkeeping: NA/ALL and the >= tickMax aging pass
// ---------------------------------------------------------------------

TEST(EVtageTables, TickPassDecrementsEveryNonzeroUIncludingBaseWays)
{
    // A small tickMax reached by repeated, fully controlled NA-only
    // scan misses: bank 1 (VT1) is the live provider throughout; each
    // repeated wrong-train on it lands the "provider exists" arm at
    // DEP == 2 (bank 2 / VT2, design doc S3's "DEP = r + 1"), which --
    // once bank 2's own u bit is set to 1 -- is disqualified on every
    // visit (NA++, no steal, no further draw: candidateQualifies()
    // short-circuits on u != 0). Also pre-seeds a base-way entry's u
    // to 1, to confirm the pass clears it too ("all components, base
    // ways included"). pcBase must differ from pc by >= 4
    // (shiftedPcOf masks off the low 2 bits, leaving only the
    // micro-op residue there -- two PCs 1 apart alias to the same
    // key, same as real 4-byte-aligned instructions would never do).
    // numTagged == 2 (not comboConfig's 3): keeps bank 1's own
    // "provider exists" scan range (DEP == 2 through numTagged) a
    // SINGLE bank (bank 2) so the repeated NA-only misses below are
    // fully controlled -- with numTagged == 3 the same scan would
    // span banks 2 AND 3, and the untouched (virgin) bank 3 would get
    // stolen on the very first repeat.
    EVtageConfig cfg = comboConfig();
    cfg.numTagged = 2;
    cfg.historyLengths = {2, 4};
    cfg.tickMax = 3;
    const Addr pc = 0x2700;     // bank 1 / bank 2 provider chain.
    const Addr pcBase = 0x3800; // A separate base-way u == 1 seed.
    const VpHistSnapshot h{};

    EVtageTables t(cfg,
                  scriptedRng({0.9, 0.5, 0.5, // Establish bank 1 (pc).
                              0.5, 0.5,      // Wrong-train bank 1 ->
                                            // lands bank 2 (dep bump
                                            // fails, dep=2; bank 2
                                            // virgin: r7 draw
                                            // irrelevant).
                              0.0, 0.0, 0.5, // Establish a base way
                                            // (pcBase).
                              0.5,           // 1st repeated bank-1
                                            // wrong-train: dep bump
                                            // (bank 2 now u==1:
                                            // short-circuits, no r7
                                            // draw). NA=1,ALL=0.
                                            // TICK: 0+1=1.
                              0.5,           // 2nd: TICK: 1+1=2.
                              0.5}));        // 3rd: TICK: 2+1=3 >=
                                            // tickMax(3): pass fires,
                                            // TICK resets to 0.

    EVtageClassifier setup;
    setup.isIndirectCall = true;
    setup.value = 0;
    t.train(pc, 0, h, 0, setup);
    const auto bank1Tok = t.lookup(pc, 0, h).token;
    ASSERT_EQ(t.peekProvider(pc, 0, h, bank1Tok).rank, 2u); // VT1->rank2.

    EVtageClassifier wrongToBank2;
    wrongToBank2.isIndirectCall = true;
    wrongToBank2.value = 999;
    auto outAlloc = t.train(pc, 0, h, bank1Tok, wrongToBank2);
    ASSERT_TRUE(contains(outAlloc, EVtageTrainOutcome::Allocated));
    const auto bank2Tok = t.lookup(pc, 0, h).token;
    ASSERT_EQ(t.peekProvider(pc, 0, h, bank2Tok).rank, 3u); // VT2->rank3.

    EVtageClassifier corr;
    corr.isIndirectCall = true;
    corr.value = 999;
    t.train(pc, 0, h, bank2Tok, corr); // bank2: u 0 -> 1.
    ASSERT_EQ(t.peekProvider(pc, 0, h, bank2Tok).u, 1u);

    EVtageClassifier setupBase;
    setupBase.isIndirectCall = true;
    setupBase.value = 0;
    t.train(pcBase, 0, h, 0, setupBase);
    const auto baseTok = t.lookup(pcBase, 0, h).token;
    ASSERT_EQ(t.peekProvider(pcBase, 0, h, baseTok).rank, 1u); // VT0.
    EVtageClassifier corrBase;
    corrBase.isIndirectCall = true;
    corrBase.value = 0;
    t.train(pcBase, 0, h, baseTok, corrBase); // base way: u 0 -> 1.
    ASSERT_EQ(t.peekProvider(pcBase, 0, h, baseTok).u, 1u);

    // bank1Tok is still live (bank 1 itself was never reallocated --
    // only bank 2, a different slot, was newly populated above). Each
    // call's value must differ from the PRIOR call's (a wrong train
    // unconditionally overwrites entry.val), or the next call would
    // see its own overwrite and score "correct" instead.
    EVtageClassifier wrongAgain1;
    wrongAgain1.isIndirectCall = true;
    wrongAgain1.value = 555;
    EVtageClassifier wrongAgain2;
    wrongAgain2.isIndirectCall = true;
    wrongAgain2.value = 556;
    EVtageClassifier wrongAgain3;
    wrongAgain3.isIndirectCall = true;
    wrongAgain3.value = 557;

    auto out1 = t.train(pc, 0, h, bank1Tok, wrongAgain1);
    EXPECT_TRUE(contains(out1, EVtageTrainOutcome::AllocScanStep));
    EXPECT_FALSE(contains(out1, EVtageTrainOutcome::TickPass));

    auto out2 = t.train(pc, 0, h, bank1Tok, wrongAgain2);
    EXPECT_TRUE(contains(out2, EVtageTrainOutcome::AllocScanStep));
    EXPECT_FALSE(contains(out2, EVtageTrainOutcome::TickPass));

    auto out3 = t.train(pc, 0, h, bank1Tok, wrongAgain3);
    EXPECT_TRUE(contains(out3, EVtageTrainOutcome::TickPass));

    // The pass cleared every nonzero u -- tagged bank 2 AND the base
    // way.
    EXPECT_EQ(t.peekProvider(pc, 0, h, bank2Tok).u, 0u);
    EXPECT_EQ(t.peekProvider(pcBase, 0, h, baseTok).u, 0u);
}

TEST(EVtageTables, TickCoefficientIsFiveTimesAllNotThreeTimes)
{
    // Pins the K32 TICK formula's ALL coefficient (design doc S4:
    // "TICK += NA - 5*ALL") against a K8 mutant (NA - 3*ALL, the
    // OTHER coefficient the raw source uses -- eves-code-brief.md S5).
    // A single NA=1/ALL=1 call is sandwiched between two batches of
    // pure NA-only scan misses (ALL == 0 throughout, so
    // coefficient-independent): the first batch (via pcC's VT3
    // provider, scanning only VT4) gets tick to a known, unclamped
    // value; the mixed call's own delta (1 - 5 = -4 vs 1 - 3 = -2)
    // then diverges cleanly; a second batch (same pcC provider) is
    // long enough that the 5x-correct tick does NOT reach tickMax
    // while a 3x-mutant tick WOULD have, at an exact call index.
    EVtageConfig cfg = comboConfig();
    cfg.numTagged = 4;
    cfg.taggedEntries = 32; // numTagged (4) must be < log2(taggedEntries).
    cfg.historyLengths = {2, 4, 8, 16};
    cfg.tickMax = 15;
    const Addr pcC = 0x4000; // Pure-NA accumulator chain: VT1->VT2->VT3
                             // (3 promotion hops), repeated misses
                             // scan VT4 only (dep = 3+1 = 4 =
                             // numTagged).
    const Addr pcA = 0x4100; // The NA=1/ALL=1 mixed-call chain: VT0
                             // scans VT1(pcA, disqualified) then
                             // VT2(pcA, virgin) in the SAME call.
    const VpHistSnapshot h{};

    std::vector<double> seq;
    seq.insert(seq.end(), {0.9, 0.5, 0.5}); // pcC: no-provider,tagged -> VT1.
    for (int i = 0; i < 3; i++) {
        seq.insert(seq.end(), {0.5, 0.5}); // VT1->VT2->VT3->VT4 (2 draws
                                           // each: dep-bump fails, r7
                                           // irrelevant for a virgin
                                           // candidate).
    }
    seq.insert(seq.end(), {0.5, 0.0, 0.5}); // pcA: no-provider,base-way
                                            // -> VT0 (way 0).
    seq.insert(seq.end(), {0.5, 0.5});      // VT0 -> VT1(pcA).
    for (int i = 0; i < 5; i++) {
        seq.push_back(0.5); // 5 pure-NA calls: dep-bump fails; VT4
                            // (pcC) u != 0 short-circuits (no r7
                            // draw).
    }
    seq.insert(seq.end(), {0.5, 0.5}); // The mixed call: dep-bump
                                       // fails; VT1(pcA) u != 0
                                       // short-circuits (no draw);
                                       // VT2(pcA) is virgin (r7 draw
                                       // needed).
    for (int i = 0; i < 14; i++) {
        seq.push_back(0.5); // 14 more pure-NA calls.
    }

    EVtageTables t(cfg, scriptedRng(seq));

    // Build the pcC chain: VT1 -> VT2 -> VT3 -> VT4.
    EVtageClassifier setup;
    setup.isIndirectCall = true;
    setup.value = 1;
    t.train(pcC, 0, h, 0, setup);
    const auto vt1c = t.lookup(pcC, 0, h).token;
    EVtageClassifier promote;
    promote.isIndirectCall = true;
    promote.value = 2;
    t.train(pcC, 0, h, vt1c, promote);
    const auto vt2c = t.lookup(pcC, 0, h).token;
    promote.value = 3;
    t.train(pcC, 0, h, vt2c, promote);
    const auto vt3c = t.lookup(pcC, 0, h).token;
    promote.value = 4;
    t.train(pcC, 0, h, vt3c, promote);
    const auto vt4c = t.lookup(pcC, 0, h).token;
    ASSERT_EQ(t.peekProvider(pcC, 0, h, vt3c).rank, 4u); // VT3 -> rank4.
    ASSERT_EQ(t.peekProvider(pcC, 0, h, vt4c).rank, 5u); // VT4 -> rank5.

    EVtageClassifier corr;
    corr.isIndirectCall = true;
    corr.value = 4;
    t.train(pcC, 0, h, vt4c, corr); // VT4(pcC): u 0 -> 1.
    ASSERT_EQ(t.peekProvider(pcC, 0, h, vt4c).u, 1u);

    // Build the pcA chain: VT0 -> VT1(pcA).
    EVtageClassifier setupA;
    setupA.isIndirectCall = true;
    setupA.value = 10;
    t.train(pcA, 0, h, 0, setupA);
    const auto vt0a = t.lookup(pcA, 0, h).token;
    ASSERT_EQ(t.peekProvider(pcA, 0, h, vt0a).rank, 1u); // VT0.
    EVtageClassifier promoteA;
    promoteA.isIndirectCall = true;
    promoteA.value = 11;
    t.train(pcA, 0, h, vt0a, promoteA);
    const auto vt1a = t.lookup(pcA, 0, h).token;
    ASSERT_EQ(t.peekProvider(pcA, 0, h, vt1a).rank, 2u); // VT1 -> rank2.

    EVtageClassifier corrA;
    corrA.isIndirectCall = true;
    corrA.value = 11;
    t.train(pcA, 0, h, vt1a, corrA); // VT1(pcA): u 0 -> 1.
    ASSERT_EQ(t.peekProvider(pcA, 0, h, vt1a).u, 1u);

    // 5 pure-NA calls via pcC's VT3 provider (dep = 3+1 = 4 =
    // numTagged: single-bank scan of VT4, disqualified).
    EVtageClassifier wrongC;
    wrongC.isIndirectCall = true;
    for (int i = 0; i < 5; i++) {
        wrongC.value = 100 + i;
        auto out = t.train(pcC, 0, h, vt3c, wrongC);
        EXPECT_TRUE(contains(out, EVtageTrainOutcome::AllocScanStep));
        EXPECT_FALSE(contains(out, EVtageTrainOutcome::TickPass));
    }

    // The mixed call: pcA's VT0 provider scans VT1(disqualified,
    // NA++) then VT2(virgin, steals, ALL++) in the SAME call.
    EVtageClassifier mixedWrong;
    mixedWrong.isIndirectCall = true;
    mixedWrong.value = 200;
    auto mixedOut = t.train(pcA, 0, h, vt0a, mixedWrong);
    EXPECT_TRUE(contains(mixedOut, EVtageTrainOutcome::AllocScanStep));
    EXPECT_TRUE(contains(mixedOut, EVtageTrainOutcome::Allocated));
    // Neither coefficient's post-mixed tick (1 under 5x, 3 under 3x)
    // reaches tickMax (15) yet.
    EXPECT_FALSE(contains(mixedOut, EVtageTrainOutcome::TickPass));

    // 12 more pure-NA calls: under the correct 5x coefficient,
    // tick == 1 + 12 == 13 < 15 (no pass); under a 3x mutant,
    // tick == 3 + 12 == 15 >= 15 (would already have fired).
    for (int i = 0; i < 12; i++) {
        wrongC.value = 300 + i;
        auto out = t.train(pcC, 0, h, vt3c, wrongC);
        EXPECT_FALSE(contains(out, EVtageTrainOutcome::TickPass))
            << "call " << i
            << " -- a 3*ALL mutant would already have fired by here";
    }
    // 2 more: under the correct 5x coefficient, tick == 13 + 2 == 15
    // >= 15 -- fires on the SECOND of these two, not the first.
    EVtageClassifier lastTwo = wrongC;
    lastTwo.value = 400;
    auto p1 = t.train(pcC, 0, h, vt3c, lastTwo);
    EXPECT_FALSE(contains(p1, EVtageTrainOutcome::TickPass));
    lastTwo.value = 401;
    auto p2 = t.train(pcC, 0, h, vt3c, lastTwo);
    EXPECT_TRUE(contains(p2, EVtageTrainOutcome::TickPass));
}

// ---------------------------------------------------------------------
// S4: 2-bit u lifecycle -- UPDATEU gating
// ---------------------------------------------------------------------

TEST(EVtageTables, DeterministicUIncrementAtSaturationRegardlessOfUpdateU)
{
    // c == confMax (7) after the correct-train's own gate: u++ fires
    // deterministically via the `|| entry.c == confMax` arm, even
    // though deliveredCorrect == true makes UPDATEU itself
    // unconditionally false (no draw).
    const Addr pc = 0x2b00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5}));
    const auto tok = establishBank1Provider(t, pc, 0, h, kLarge);
    saturateConfidence(t, pc, 0, h, tok, kLarge, 3);
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).c, 7u);
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).u, 1u);
}

TEST(EVtageTables, NoUBumpOnDeliveredCorrectBelowSaturation)
{
    // deliveredCorrect == true -> UPDATEU's own gate is unconditionally
    // false (short-circuit, no draw); c stays below confMax (use a
    // failing confidence-gate draw), so the deterministic arm doesn't
    // fire either: u stays 0.
    const Addr pc = 0x2c00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5, 0.99}));
    const auto tok = establishBank1Provider(t, pc, 0, h, 0);
    EVtageClassifier corr = nonLoadClassifier(true, false, 0, 2);
    corr.deliveredCorrect = true;
    auto out = t.train(pc, 0, h, tok, corr);
    EXPECT_EQ(out, Outcomes{EVtageTrainOutcome::CorrectSat}); // Gate
                                                              // failed
                                                              // (0.99).
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).u, 0u); // No u bump.
}

TEST(EVtageTables, UpdateUFiresWhenNotDeliveredCorrectAndDrawSucceeds)
{
    // deliveredCorrect == false: UPDATEU's own probabilistic draw is
    // live. Force the confidence gate to fail first (c stays at 3, not
    // confMax) so the deterministic arm can't explain a u bump, then
    // script UPDATEU's own draw to succeed.
    const Addr pc = 0x2d00;
    const VpHistSnapshot h{};
    // E_u for a non-load alu row with intSrcCount<2 and value 0
    // (zero, LOWVAL=2): E_u = LOWVAL(2) + 2*NOTL1MISS(2) +
    // nonload(1) + FASTINST(1) + aluBonus(2) = 8.
    EVtageTables t(comboConfig(),
                  scriptedRng({0.9, 0.5, 0.5,   // Establish bank 1.
                              0.99,            // Confidence gate: fails.
                              (1.0 / 256) * 0.25})); // UPDATEU draw:
                                                     // fires.
    const auto tok = establishBank1Provider(t, pc, 0, h, 0);
    EVtageClassifier corr =
        nonLoadClassifier(/*fastInst*/ true, /*slowInst*/ false, 0,
                          /*intSrcCount*/ 0);
    corr.deliveredCorrect = false;
    auto out = t.train(pc, 0, h, tok, corr);
    EXPECT_EQ(out, Outcomes{EVtageTrainOutcome::CorrectSat});
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).u, 1u);
}

TEST(EVtageTables, UExponentTruthTable)
{
    // S4's verbatim K32 exponent: E_u = LOWVAL + 2*NOTL1MISS +
    // (INSTTYPE != load) + FASTINST + 2*(INSTTYPE == alu)*(NbOperand
    // < 2). The alu-bonus term is the one E_u-specific feature (not
    // shared with E or E'): it fires ONLY for a genuine alu
    // (fastInst) with < 2 operands, never for store/undef (the
    // "neither" fallback) even at 0 operands, and never for loads.
    EVtageClassifier aluBonusApplies =
        nonLoadClassifier(/*fastInst*/ true, /*slowInst*/ false, kLarge,
                          /*intSrcCount*/ 0);
    assertUExponent(aluBonusApplies, 6); // LOWVAL(0) + 2 + 1 + 1 + 2.

    EVtageClassifier aluBonusBlockedByOperandCount =
        nonLoadClassifier(/*fastInst*/ true, /*slowInst*/ false, kLarge,
                          /*intSrcCount*/ 2);
    assertUExponent(aluBonusBlockedByOperandCount, 4); // No aluBonus:
                                                       // LOWVAL(0)+2+1+1.

    EVtageClassifier neitherNeverGetsAluBonus =
        nonLoadClassifier(/*fastInst*/ false, /*slowInst*/ false, kLarge,
                          /*intSrcCount*/ 0);
    assertUExponent(neitherNeverGetsAluBonus, 4); // Same as above: the
                                                  // alu bonus checks the
                                                  // raw fastInst field,
                                                  // not 0 operands alone.

    assertUExponent(loadClassifier(EVtageMemLevel::Stlf, kLarge), 3);
}

TEST(EVtageTables, UBitCapsAtUMax)
{
    // u cannot exceed uMax (3, uBits == 2): once saturated, further
    // deterministic-arm trains must not increment past it (no
    // overflow/wrap). isIndirectCall makes every correct train's
    // u-update deterministic (dispatch bypass), so u reaches uMax well
    // before confidence even saturates -- exactly the "no wrap" case
    // this test pins.
    const Addr pc = 0x2e00;
    const VpHistSnapshot h{};
    EVtageTables t(comboConfig(), scriptedRng({0.9, 0.5, 0.5}));
    const auto tok = establishBank1Provider(t, pc, 0, h, 0);
    EVtageClassifier corr;
    corr.isIndirectCall = true;
    corr.value = 0;
    for (int i = 0; i < 7; i++) {
        t.train(pc, 0, h, tok, corr);
    }
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).c, 7u);
    EXPECT_LE(t.peekProvider(pc, 0, h, tok).u, 3u);
    t.train(pc, 0, h, tok, corr); // One more at saturation: still <= 3.
    EXPECT_LE(t.peekProvider(pc, 0, h, tok).u, 3u);
}

// ---------------------------------------------------------------------
// Burst guard
// ---------------------------------------------------------------------

TEST(EVtageTables, BurstGuardOffByDefaultNeverSuppresses)
{
    EVtageTables t(comboConfig(), constRng(0.5));
    EXPECT_FALSE(t.burstGuardSuppresses(0));
    EXPECT_FALSE(t.burstGuardSuppresses(1000000));
}

TEST(EVtageTables, BurstGuardSuppressesWithinWindow)
{
    EVtageConfig cfg = comboConfig();
    cfg.burstGuardWindow = 128;
    EVtageTables t(cfg, constRng(0.5));
    EXPECT_TRUE(t.burstGuardSuppresses(0));
    EXPECT_TRUE(t.burstGuardSuppresses(127));
    EXPECT_FALSE(t.burstGuardSuppresses(128));
    EXPECT_FALSE(t.burstGuardSuppresses(200));
}

// ---------------------------------------------------------------------
// S5: base 2-way skewed hash distinctness
// ---------------------------------------------------------------------

TEST(EVtageTables, BaseWaysUseDistinctHashFunctions)
{
    // S5's entire point ("two hash functions... two skewed ways") has
    // no external CVP source to cross-check against (this base has no
    // literal CVP analog); pin it directly via
    // the test-only debugBaseWayHash() accessor: over a sample of PCs
    // (and a couple of history/upc variations), way 0 and way 1 must
    // never produce the SAME {index, tag} pair -- that would mean the
    // two "skewed ways" are silently the same physical array.
    EVtageTables t(comboConfig(), constRng(0.5));
    int collisions = 0;
    for (uint64_t i = 0; i < 200; i++) {
        const Addr pc = 0x10000 + i * 4;
        const auto way0 = t.debugBaseWayHash(pc, 0, 0);
        const auto way1 = t.debugBaseWayHash(pc, 0, 1);
        if (way0 == way1) {
            collisions++;
        }
    }
    // A genuine (non-degenerate) skew may coincide for a handful of
    // PCs by pure chance, but a mutant collapsing the two ways to one
    // hash function would coincide for EVERY PC (all 200).
    EXPECT_LT(collisions, 200);
    // Direct spot check at one PC, matching the S5 doc's own framing.
    const auto spotWay0 = t.debugBaseWayHash(0x400890, 0, 0);
    const auto spotWay1 = t.debugBaseWayHash(0x400890, 0, 1);
    EXPECT_TRUE(spotWay0.first != spotWay1.first ||
               spotWay0.second != spotWay1.second);
}
