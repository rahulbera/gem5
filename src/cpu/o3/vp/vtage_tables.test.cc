#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <vector>

#include "cpu/o3/vp/vtage_tables.hh"

using namespace gem5;
using namespace gem5::o3;

namespace
{

using Outcomes = std::vector<VtageTrainOutcome>;

/** Config for the "combo" scenario: 3 tagged components, small enough
 *  to hand-verify. numTagged (3) < log2(taggedEntries) (4), the
 *  geometry the F() path hash requires. */
VtageConfig
comboConfig()
{
    VtageConfig c;
    c.baseEntries = 16;
    c.taggedEntries = 16;
    c.numTagged = 3;
    c.historyLengths = {2, 4, 8};
    c.baseTagBits = 8;
    c.confBits = 3;
    c.confThreshold = 7;
    c.fpcVector = {1, 1. / 16, 1. / 16, 1. / 16, 1. / 16, 1. / 32, 1. / 32};
    c.pathBits = 16;
    return c;
}

/** Config for single-tagged-component scenarios (threshold gating,
 *  FPC, wrong/overwrite, aging, corrective reset). */
VtageConfig
singleBankConfig(unsigned taggedEntries, unsigned confThreshold)
{
    VtageConfig c;
    c.baseEntries = 16;
    c.taggedEntries = taggedEntries;
    c.numTagged = 1;
    c.historyLengths = {2};
    c.baseTagBits = 8;
    c.confBits = 3;
    c.confThreshold = confThreshold;
    c.fpcVector = {1, 1. / 16, 1. / 16, 1. / 16, 1. / 16, 1. / 32, 1. / 32};
    c.pathBits = 16;
    return c;
}

/** A constant rng: returns the same value every draw. */
std::function<double()>
constRng(double v)
{
    return [v]() { return v; };
}

/** A scripted rng: returns successive values from a fixed sequence.
 *  Each value may only be drawn once (a test that over-draws has
 *  mis-modeled how many rng() calls a train() makes). */
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

} // namespace

TEST(VtageTables, ColdLookupFallsBackToVT0)
{
    VtageTables t(comboConfig(), constRng(0.0));
    auto r = t.lookup(0x400, 0, {});
    EXPECT_TRUE(r.hit); // VT0 always "hits".
    EXPECT_FALSE(r.confident);
    EXPECT_EQ(r.value, 0u);
    EXPECT_NE(r.token, 0u); // Biased rank: never 0 for a real lookup.
}

TEST(VtageTables, SameSnapshotGivesSameToken)
{
    VtageTables t(comboConfig(), constRng(0.0));
    VpHistSnapshot h{0x1234, 7};
    auto r1 = t.lookup(0x400, 0, h);
    auto r2 = t.lookup(0x400, 0, h);
    EXPECT_EQ(r1.token, r2.token);
}

// The "combo" scenario populates VT0, VT1 (bank 1, L=2) and VT3
// (bank 3, L=8) with distinct values at ghr=0, via three chained
// wrong-trainings (each cascades the allocation ladder one step
// further). Bit sensitivity is then read off by flipping ghr bits at
// specific positions relative to the configured history lengths
// {2, 4, 8}: flipping bit 3 (inside L=4 and L=8's windows, outside
// L=2's) should demote the provider from VT3 down to VT1; flipping
// bit 0 (inside every window) should demote it all the way to VT0.
// Hand/model-verified: see .superpowers/sdd (task 1 derivation).
TEST(VtageTables, LongestMatchWinsThenBitFlipsRevealShorterProviders)
{
    VtageTables t(comboConfig(), scriptedRng({0.0, 0.9}));
    const Addr pc = 0x400;
    const VpHistSnapshot h0{0, 0};

    auto out1 = t.train(pc, 0, h0, 0, 111);
    EXPECT_EQ(out1, (Outcomes{VtageTrainOutcome::StaleTokenRecomputed,
                              VtageTrainOutcome::WrongValOverwrite,
                              VtageTrainOutcome::Allocated}));

    auto out2 = t.train(pc, 0, h0, 0, 222);
    EXPECT_EQ(out2, (Outcomes{VtageTrainOutcome::StaleTokenRecomputed,
                              VtageTrainOutcome::WrongValOverwrite,
                              VtageTrainOutcome::Allocated}));

    // Bank 3 (L = 8) is now the provider (topmost): no candidates
    // remain above it, so no allocation fires.
    auto out3 = t.train(pc, 0, h0, 0, 333);
    EXPECT_EQ(out3, (Outcomes{VtageTrainOutcome::StaleTokenRecomputed,
                              VtageTrainOutcome::WrongValOverwrite}));

    // Longest match wins: bank 3's value.
    EXPECT_EQ(t.lookup(pc, 0, h0).value, 333u);

    // Flip bit 3 (< L=4, L=8; NOT < L=2): bank 3 and bank 2 (virgin
    // anyway) miss, bank 1 (L = 2) still matches -> its value.
    const VpHistSnapshot hBit3{h0.ghr ^ (1ull << 3), 0};
    EXPECT_EQ(t.lookup(pc, 0, hBit3).value, 222u);

    // Flip bit 0 (< every configured L): every tagged bank misses ->
    // falls all the way back to VT0.
    const VpHistSnapshot hBit0{h0.ghr ^ (1ull << 0), 0};
    EXPECT_EQ(t.lookup(pc, 0, hBit0).value, 111u);
}

// shiftedPcOf's trailing "XOR upc" is what actually delivers micro-op
// separation into the hashed, masked tables (the pre-fix formula was
// arithmetically inert here: upc lives in bits [46:62) of the
// pre-shift value, which never survive the low masked bits any
// index/tag keeps). Two micro-ops of the same macro-op must get
// distinct entries and independently reach their own confidence
// holding their own values.
TEST(VtageTables, MicroPcSeparation)
{
    VtageTables t(singleBankConfig(16, 2), constRng(0.0));
    const Addr pc = 0x400890;
    const VpHistSnapshot h{};

    const auto virgin0 = t.lookup(pc, 0, h).token;
    const auto virgin1 = t.lookup(pc, 1, h).token;
    EXPECT_NE(virgin0, virgin1);

    t.train(pc, 0, h, virgin0, 111); // Wrong (val == 0): allocates.
    t.train(pc, 1, h, virgin1, 222); // Wrong (val == 0): allocates.

    const auto tok0 = t.lookup(pc, 0, h).token;
    const auto tok1 = t.lookup(pc, 1, h).token;
    EXPECT_NE(tok0, tok1);

    t.train(pc, 0, h, tok0, 111);               // Correct: c 0 -> 1.
    t.train(pc, 1, h, tok1, 222);               // Correct: c 0 -> 1.
    EXPECT_FALSE(t.lookup(pc, 0, h).confident); // Threshold 2.
    t.train(pc, 0, h, tok0, 111);               // Correct: c 1 -> 2.
    t.train(pc, 1, h, tok1, 222);               // Correct: c 1 -> 2.

    auto ra = t.lookup(pc, 0, h);
    auto rb = t.lookup(pc, 1, h);
    EXPECT_TRUE(ra.confident);
    EXPECT_EQ(ra.value, 111u);
    EXPECT_TRUE(rb.confident);
    EXPECT_EQ(rb.value, 222u);
    EXPECT_NE(ra.token, rb.token);
}

TEST(VtageTables, TrainWithTokenZeroRecomputesProvider)
{
    VtageTables t(singleBankConfig(16, 7), constRng(0.0));
    auto out = t.train(0x900, 0, {}, /* token */ 0, /* actual */ 0);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out[0], VtageTrainOutcome::StaleTokenRecomputed);
    // The recomputed provider is VT0 (virgin), val == 0 == actual.
    EXPECT_EQ(out[1], VtageTrainOutcome::CorrectInc);
}

TEST(VtageTables, TrainWithStaleTagTokenRecomputesProvider)
{
    // Same setup as CorrectiveResetOnStaleTagReturnsFalse: bank 1 is
    // established at pcA, then a colliding key (pc2) hijacks the same
    // cell with a different tag.
    VtageTables t(singleBankConfig(4, 7), constRng(0.0));
    const Addr pcA = 0x900;
    const VpHistSnapshot hA{0, 0};

    const auto virginTokA = t.lookup(pcA, 0, hA).token;
    t.train(pcA, 0, hA, virginTokA, 111); // Allocate bank 1.
    const auto staleTok = t.lookup(pcA, 0, hA).token;

    const Addr pc2 = 2308;
    const VpHistSnapshot hProbe{1, 0};
    const auto probeVirginTok = t.lookup(pc2, 0, hProbe).token;
    auto hijack = t.train(pc2, 0, hProbe, probeVirginTok, 777);
    ASSERT_EQ(hijack, (Outcomes{VtageTrainOutcome::WrongValOverwrite,
                                VtageTrainOutcome::Allocated}));

    auto out = t.train(pcA, 0, hA, staleTok, 555);
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out[0], VtageTrainOutcome::StaleTokenRecomputed);
    // The hijacked entry is untouched by pcA's stale-token retrain.
    EXPECT_EQ(t.lookup(pc2, 0, hProbe).value, 777u);
}

// Mutants that drop the oldest bit of a bank's history window (the
// last bit FoldedHistory::update folds in) are undetected by tests
// that only probe the bit at a DIFFERENT bank's boundary. This probes
// each bank's own oldest bit directly: bit 7 for L = 8 (bank 3), bit
// 1 for L = 2 (bank 1); a bit outside the window (bit 8) must have no
// effect.
TEST(VtageTables, OldestHistoryBitReachesFold)
{
    {
        VtageTables t(comboConfig(), constRng(0.9));
        const Addr pc = 0x1200;
        const VpHistSnapshot hBase{0x80, 0}; // Bit 7 set: oldest of L=8.
        const auto virginTok = t.lookup(pc, 0, hBase).token;
        // 3 virgin candidates, r = 0.9 picks the last (bank 3).
        t.train(pc, 0, hBase, virginTok, 777);
        const auto bank3Tok = t.lookup(pc, 0, hBase).token;
        EXPECT_NE(bank3Tok, virginTok);

        const VpHistSnapshot hFlip7{hBase.ghr ^ (1ull << 7), 0};
        EXPECT_NE(t.lookup(pc, 0, hFlip7).token, bank3Tok);

        const VpHistSnapshot hFlip8{hBase.ghr ^ (1ull << 8), 0};
        EXPECT_EQ(t.lookup(pc, 0, hFlip8).token, bank3Tok);
    }
    {
        VtageTables t(comboConfig(), constRng(0.0));
        const Addr pc = 0x1300;
        const VpHistSnapshot hBase{0x2, 0}; // Bit 1 set: oldest of L=2.
        const auto virginTok = t.lookup(pc, 0, hBase).token;
        // 3 virgin candidates, r = 0.0 picks the first (bank 1).
        t.train(pc, 0, hBase, virginTok, 555);
        const auto bank1Tok = t.lookup(pc, 0, hBase).token;
        EXPECT_NE(bank1Tok, virginTok);

        const VpHistSnapshot hFlip1{hBase.ghr ^ (1ull << 1), 0};
        EXPECT_NE(t.lookup(pc, 0, hFlip1).token, bank1Tok);
    }
}

// Regression pins for the fold/index/tag/pack recurrences: exact
// packed token values for fixed (pc, upc, snapshot) triples on a
// virgin table at the default (HPCA'14) geometry, where every lookup
// falls back to VT0. Golden values were captured by running this
// exact code once, after the shiftedPcOf fix; their purpose is drift
// detection on the fold/index/tag/pack arithmetic, not independent
// derivation.
TEST(VtageTables, GoldenTokens)
{
    VtageTables t(VtageConfig{}, constRng(0.0));
    EXPECT_EQ(t.lookup(0x400000, 0, {}).token, 1ull);
    EXPECT_EQ(t.lookup(0x400000, 1, {}).token, 17ull);
    EXPECT_EQ(
        t.lookup(0x400004, 0, VpHistSnapshot{0x123456789abcdef0ull, 0x1234})
            .token,
        17ull);
}

TEST(VtageTables, ThresholdGatingDefaultSaturation)
{
    VtageTables t(singleBankConfig(16, 7), constRng(0.0));
    const Addr pc = 0x100;
    const VpHistSnapshot h{};
    const auto tok = t.lookup(pc, 0, h).token;
    for (int i = 0; i < 6; i++) {
        t.train(pc, 0, h, tok, 0); // val == 0 == actual: correct.
    }
    EXPECT_FALSE(t.lookup(pc, 0, h).confident); // c == 6 < 7.
    t.train(pc, 0, h, tok, 0);
    EXPECT_TRUE(t.lookup(pc, 0, h).confident); // c == 7 == threshold.

    // Saturation holds: one more correct train at c == 7 can only
    // report CorrectSat (no transition past the counter's max), and
    // confidence must not wrap back down (a wrap-to-0 mutant passed
    // the suite before this was pinned).
    EXPECT_EQ(t.train(pc, 0, h, tok, 0),
              Outcomes{VtageTrainOutcome::CorrectSat});
    EXPECT_TRUE(t.lookup(pc, 0, h).confident);
}

TEST(VtageTables, ThresholdGatingConfThresholdOnePredicts)
{
    VtageTables t(singleBankConfig(16, 1), constRng(0.0));
    const Addr pc = 0x100;
    const VpHistSnapshot h{};
    const auto tok = t.lookup(pc, 0, h).token;
    t.train(pc, 0, h, tok, 0); // c: 0 -> 1.
    EXPECT_TRUE(t.lookup(pc, 0, h).confident);
}

TEST(VtageTables, FpcHighRngOnlyGuaranteedTransitionFires)
{
    // v = {1, 1/16, ...}: rng() == 0.99 clears only the 0 -> 1 gate
    // (v[0] == 1); every later gate (<= 1/16) fails it. With
    // confThreshold = 2, confidence can therefore never arrive.
    VtageTables t(singleBankConfig(16, 2), constRng(0.99));
    const Addr pc = 0x100;
    const VpHistSnapshot h{};
    const auto tok = t.lookup(pc, 0, h).token;

    EXPECT_EQ(t.train(pc, 0, h, tok, 0),
              Outcomes{VtageTrainOutcome::CorrectInc});
    EXPECT_FALSE(t.lookup(pc, 0, h).confident);
    EXPECT_EQ(t.train(pc, 0, h, tok, 0),
              Outcomes{VtageTrainOutcome::CorrectSat});
    EXPECT_EQ(t.train(pc, 0, h, tok, 0),
              Outcomes{VtageTrainOutcome::CorrectSat});
    EXPECT_FALSE(t.lookup(pc, 0, h).confident); // c stuck at 1 < 2.
}

// numTagged = 1 keeps the sole tagged component topmost: its own
// wrong-training never triggers an allocation probe, so the outcome
// vector stays a clean single element.
TEST(VtageTables, WrongResetPreservesValueWhenConfidencePositive)
{
    VtageTables t(singleBankConfig(16, 7), constRng(0.0));
    const Addr pc = 0x2000;
    const VpHistSnapshot h{};

    const auto tok0 = t.lookup(pc, 0, h).token;
    t.train(pc, 0, h, tok0, 111); // VT0 wrong -> allocates bank 1.
    const auto tok1 = t.lookup(pc, 0, h).token;
    ASSERT_EQ(t.lookup(pc, 0, h).value, 111u);

    EXPECT_EQ(t.train(pc, 0, h, tok1, 111),
              Outcomes{VtageTrainOutcome::CorrectInc}); // c: 0 -> 1.

    EXPECT_EQ(t.train(pc, 0, h, tok1, 99),
              Outcomes{VtageTrainOutcome::WrongReset});
    EXPECT_EQ(t.lookup(pc, 0, h).value, 111u); // Unchanged.
    EXPECT_FALSE(t.lookup(pc, 0, h).confident);
}

TEST(VtageTables, WrongOverwritesValueWhenConfidenceIsZero)
{
    VtageTables t(singleBankConfig(16, 7), constRng(0.0));
    const Addr pc = 0x2000;
    const VpHistSnapshot h{};

    const auto tok0 = t.lookup(pc, 0, h).token;
    t.train(pc, 0, h, tok0, 111); // Allocate bank 1, c == 0.
    const auto tok1 = t.lookup(pc, 0, h).token;

    EXPECT_EQ(t.train(pc, 0, h, tok1, 99),
              Outcomes{VtageTrainOutcome::WrongValOverwrite});
    EXPECT_EQ(t.lookup(pc, 0, h).value, 99u);
}

TEST(VtageTables, WrongResetIsUnconditionalRegardlessOfRng)
{
    for (double rv : {0.0, 0.99}) {
        VtageTables t(singleBankConfig(16, 7), constRng(rv));
        const Addr pc = 0x2000;
        const VpHistSnapshot h{};
        const auto tok0 = t.lookup(pc, 0, h).token;
        t.train(pc, 0, h, tok0, 111);
        const auto tok1 = t.lookup(pc, 0, h).token;
        t.train(pc, 0, h, tok1, 111); // c: 0 -> 1.

        EXPECT_EQ(t.train(pc, 0, h, tok1, 99),
                  Outcomes{VtageTrainOutcome::WrongReset});
        EXPECT_EQ(t.lookup(pc, 0, h).value, 111u) << "rng = " << rv;
    }
}

// Allocation candidate choice: 3 virgin tagged components (banks 1,
// 2, 3), rng() == 0.5 picks the middle one (floor(0.5 * 3) == 1).
// Identified from outside purely via token comparisons: flipping a
// history bit inside bank 2's window (but outside bank 1's) must
// demote the provider back to VT0; flipping a bit outside bank 2's
// window (but inside bank 3's) must NOT affect it -- together they
// rule out bank 1 and bank 3, leaving bank 2 as the only consistent
// explanation.
TEST(VtageTables, AllocationChoiceViaScriptedRngPicksMiddleCandidate)
{
    VtageTables t(comboConfig(), constRng(0.5));
    const Addr pc = 0x1000;
    const VpHistSnapshot h0{0, 0};

    const auto virginTok = t.lookup(pc, 0, h0).token;
    t.train(pc, 0, h0, virginTok, 42);
    const auto providerTok = t.lookup(pc, 0, h0).token;
    EXPECT_NE(providerTok, virginTok);
    EXPECT_EQ(t.lookup(pc, 0, h0).value, 42u);

    const VpHistSnapshot hBit2{h0.ghr ^ (1ull << 2), 0};
    EXPECT_EQ(t.lookup(pc, 0, hBit2).token, virginTok); // Falls to VT0.

    const VpHistSnapshot hBit4{h0.ghr ^ (1ull << 4), 0};
    EXPECT_EQ(t.lookup(pc, 0, hBit4).token, providerTok); // Unaffected.
}

// u == correct: a provider trained correct (u -> 1) must be excluded
// from a later allocation probe that happens to compute its way to
// the very same cell from a different (pc, history) key -- observed
// purely by re-lookup: the excluded entry's token/value must not
// change, while some other candidate absorbs the allocation.
TEST(VtageTables, AllocationSkipsCandidateWithUsefulBitSet)
{
    VtageTables t(comboConfig(), scriptedRng({0.0, 0.9, 0.0, 0.0}));
    const Addr pc = 0x400;
    const VpHistSnapshot h0{0, 0};
    t.train(pc, 0, h0, 0, 111); // -> bank 1 allocated (via bank1 pick)
    t.train(pc, 0, h0, 0, 222); // -> bank 3 allocated, bank 1 shadowed

    // Bank 1 (L = 2) is reachable again at ghr ^ bit3 (outside its
    // window, inside bank 3's -- see the LongestMatchWins test).
    const VpHistSnapshot hBit3{h0.ghr ^ (1ull << 3), 0};
    const auto bank1Tok = t.lookup(pc, 0, hBit3).token;
    ASSERT_EQ(t.lookup(pc, 0, hBit3).value, 222u);

    // Correct-train bank 1 in place: u -> 1.
    EXPECT_EQ(t.train(pc, 0, hBit3, bank1Tok, 222),
              Outcomes{VtageTrainOutcome::CorrectInc});

    // A different (pc, ghr) pair whose own bank-1 probe index
    // coincides with bank1Tok's cell (hand-derived collision), but
    // whose tag differs, so its own lookup falls straight to VT0.
    const Addr pc2 = 1088;
    const VpHistSnapshot hProbe{1, 0};
    ASSERT_NE(t.lookup(pc2, 0, hProbe).token, bank1Tok);
    const auto probeVirginTok = t.lookup(pc2, 0, hProbe).token;

    auto out = t.train(pc2, 0, hProbe, probeVirginTok, 999);
    EXPECT_EQ(out, (Outcomes{VtageTrainOutcome::WrongValOverwrite,
                             VtageTrainOutcome::Allocated}));
    EXPECT_EQ(t.lookup(pc2, 0, hProbe).value, 999u);

    // Bank 1's own (pc, hBit3) entry must be untouched: excluded from
    // the candidate set because its u bit was set.
    EXPECT_EQ(t.lookup(pc, 0, hBit3).token, bank1Tok);
    EXPECT_EQ(t.lookup(pc, 0, hBit3).value, 222u);
}

// Variant of AllocationSkipsCandidateWithUsefulBitSet: this time the
// u == 1 entry itself is trained WRONG (not probed from elsewhere).
// The wrong training must clear its own u bit, making it reclaimable
// by a later colliding allocation -- and the reclaim must actually
// evict the old provider (observed via token, since the coincidental
// VT0-overwrite quirk can leave the *value* looking unchanged).
TEST(VtageTables, WrongTrainClearsUsefulBitMakingEntryReclaimable)
{
    VtageTables t(singleBankConfig(4, 7), constRng(0.0));
    const Addr pcA = 0x800;
    const VpHistSnapshot hA{0, 0};

    const auto virginTokA = t.lookup(pcA, 0, hA).token;
    t.train(pcA, 0, hA, virginTokA, 111); // Allocate bank 1.
    const auto bank1Tok = t.lookup(pcA, 0, hA).token;
    t.train(pcA, 0, hA, bank1Tok, 111); // Correct -> u = 1.

    // numTagged == 1: bank 1 is topmost, so training it wrong never
    // triggers its own allocation probe -- an isolated look at
    // whether u actually drops back to 0.
    EXPECT_EQ(t.train(pcA, 0, hA, bank1Tok, 222),
              Outcomes{VtageTrainOutcome::WrongReset});
    EXPECT_EQ(t.lookup(pcA, 0, hA).token, bank1Tok); // Not reallocated.

    // Same hand-derived collision as
    // AllocationAgesUsefulBitsWhenNoCandidateQualifies: pc2's own
    // bank-1 probe index coincides with bank1's cell, but its own
    // lookup misses (falls to VT0) since the tag differs.
    const Addr pc2 = 2052;
    const VpHistSnapshot hProbe{1, 0};
    const auto probeVirginTok = t.lookup(pc2, 0, hProbe).token;

    auto out = t.train(pc2, 0, hProbe, probeVirginTok, 999);
    EXPECT_EQ(out, (Outcomes{VtageTrainOutcome::WrongValOverwrite,
                             VtageTrainOutcome::Allocated}));
    EXPECT_EQ(t.lookup(pc2, 0, hProbe).value, 999u);

    // The old provider is gone: pcA's own lookup no longer finds the
    // tagged bank-1 entry (u == 0 let it be reclaimed), so its token
    // must differ from bank1Tok (it falls back to VT0).
    EXPECT_NE(t.lookup(pcA, 0, hA).token, bank1Tok);
}

// Aging: when every candidate above the provider has u == 1, no
// allocation happens and their u bits are cleared instead; a later,
// identical probe then succeeds.
TEST(VtageTables, AllocationAgesUsefulBitsWhenNoCandidateQualifies)
{
    VtageTables t(singleBankConfig(4, 7), constRng(0.0));
    const Addr pcA = 0x800;
    const VpHistSnapshot hA{0, 0};

    const auto virginTokA = t.lookup(pcA, 0, hA).token;
    t.train(pcA, 0, hA, virginTokA, 111); // Allocate bank 1.
    const auto bank1Tok = t.lookup(pcA, 0, hA).token;
    t.train(pcA, 0, hA, bank1Tok, 111); // Correct -> u = 1.

    // Hand-derived collision: pc2's own bank-1 probe index equals
    // bank1's cell but its tag differs, and pc2's VT0 cell is
    // untouched (fresh), so its own lookup is a clean VT0 fallback.
    const Addr pc2 = 2052;
    const VpHistSnapshot hProbe{1, 0};
    const auto probeVirginTok = t.lookup(pc2, 0, hProbe).token;
    ASSERT_EQ(t.lookup(pc2, 0, hProbe).value, 0u);

    auto out1 = t.train(pc2, 0, hProbe, probeVirginTok, 999);
    EXPECT_EQ(out1, (Outcomes{VtageTrainOutcome::WrongValOverwrite,
                              VtageTrainOutcome::AllocFailedAged}));
    // No allocation: still VT0 (now holding the overwritten value).
    EXPECT_EQ(t.lookup(pc2, 0, hProbe).value, 999u);
    // Bank 1 (pcA) is unaffected by the aging pass.
    EXPECT_EQ(t.lookup(pcA, 0, hA).token, bank1Tok);
    EXPECT_EQ(t.lookup(pcA, 0, hA).value, 111u);

    // Retry the identical probe: u was aged to 0, so this time the
    // allocation succeeds.
    auto out2 = t.train(pc2, 0, hProbe, probeVirginTok, 888);
    EXPECT_EQ(out2, (Outcomes{VtageTrainOutcome::WrongValOverwrite,
                              VtageTrainOutcome::Allocated}));
    EXPECT_EQ(t.lookup(pc2, 0, hProbe).value, 888u);
}

TEST(VtageTables, CorrectiveResetClearsConfidenceButNotValue)
{
    VtageTables t(singleBankConfig(16, 2), constRng(0.0));
    const Addr pc = 0x500;
    const VpHistSnapshot h{};

    const auto tok0 = t.lookup(pc, 0, h).token;
    t.train(pc, 0, h, tok0, 111); // Allocate bank 1.
    const auto tok1 = t.lookup(pc, 0, h).token;
    t.train(pc, 0, h, tok1, 111); // c: 0 -> 1.
    t.train(pc, 0, h, tok1, 111); // c: 1 -> 2 (== confThreshold).
    ASSERT_TRUE(t.lookup(pc, 0, h).confident);

    EXPECT_TRUE(t.correctiveReset(tok1));
    auto r = t.lookup(pc, 0, h);
    EXPECT_FALSE(r.confident);
    EXPECT_EQ(r.value, 111u); // No value write.
    EXPECT_EQ(r.token, tok1); // Same entry, not reallocated.
}

// The no-livelock arm for the most common provider: VT0 has no tag,
// so its corrective reset is untagged (index-only) -- pin that it
// still clears confidence without any value write or reallocation.
TEST(VtageTables, CorrectiveResetOnVt0TokenClearsBaseConfidence)
{
    VtageTables t(singleBankConfig(16, 2), constRng(0.0));
    const Addr pc = 0x300;
    const VpHistSnapshot h{};
    const auto vt0Tok = t.lookup(pc, 0, h).token;

    t.train(pc, 0, h, vt0Tok, 0); // Correct (val == 0): c 0 -> 1.
    t.train(pc, 0, h, vt0Tok, 0); // Correct: c 1 -> 2 (== threshold).
    ASSERT_TRUE(t.lookup(pc, 0, h).confident);

    EXPECT_TRUE(t.correctiveReset(vt0Tok));
    auto r = t.lookup(pc, 0, h);
    EXPECT_FALSE(r.confident);
    EXPECT_EQ(r.value, 0u);     // No value write.
    EXPECT_EQ(r.token, vt0Tok); // Same entry: VT0 has no tag to stale.
}

TEST(VtageTables, CorrectiveResetOnStaleTagReturnsFalse)
{
    VtageTables t(singleBankConfig(4, 7), constRng(0.0));
    const Addr pcA = 0x900;
    const VpHistSnapshot hA{0, 0};

    const auto virginTokA = t.lookup(pcA, 0, hA).token;
    t.train(pcA, 0, hA, virginTokA, 111); // Allocate bank 1.
    const auto staleTok = t.lookup(pcA, 0, hA).token;

    // Hand-derived collision: pc2 lands on the same bank-1 cell with
    // a different tag, and reallocates it (u == 0 there still).
    const Addr pc2 = 2308;
    const VpHistSnapshot hProbe{1, 0};
    const auto probeVirginTok = t.lookup(pc2, 0, hProbe).token;
    auto out = t.train(pc2, 0, hProbe, probeVirginTok, 777);
    ASSERT_EQ(out, (Outcomes{VtageTrainOutcome::WrongValOverwrite,
                             VtageTrainOutcome::Allocated}));
    const auto freshTok = t.lookup(pc2, 0, hProbe).token;

    EXPECT_FALSE(t.correctiveReset(staleTok));
    EXPECT_TRUE(t.correctiveReset(freshTok));
}

TEST(VtageTables, CorrectiveResetTokenZeroReturnsFalse)
{
    VtageTables t(comboConfig(), constRng(0.0));
    EXPECT_FALSE(t.correctiveReset(0));
}

TEST(VtageTables, Vt0TokenAtIndexZeroIsDistinctFromTokenZero)
{
    VtageTables t(comboConfig(), constRng(0.0));
    // pc == 0, upc == 0 folds to shiftedPc == 0, so VT0's index is 0
    // -- the "index 0" case the biased rank must still disambiguate
    // from the sentinel token value 0.
    auto r = t.lookup(0, 0, {});
    EXPECT_NE(r.token, 0u);
}

// gem5's fatal() throws (rather than aborts the process) when linked
// into a GTest binary (base/gtest/logging_mock.cc), so fatal_if
// construction-time geometry asserts show up here as C++ exceptions,
// not process death -- ASSERT_ANY_THROW is the pattern the rest of
// the tree uses for this (e.g. base/stats/info.test.cc).
TEST(VtageTables, ConfThresholdBelowOneIsFatal)
{
    VtageConfig cfg = comboConfig();
    cfg.confThreshold = 0;
    ASSERT_ANY_THROW(VtageTables(cfg, constRng(0.0)));
}

TEST(VtageTables, NumTaggedOutOfRangeIsFatal)
{
    VtageConfig cfg = comboConfig();
    cfg.numTagged = 8;
    cfg.historyLengths = {2, 4, 8, 16, 32, 64, 128, 256};
    ASSERT_ANY_THROW(VtageTables(cfg, constRng(0.0)));
}
