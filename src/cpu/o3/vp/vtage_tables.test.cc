#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <vector>

#include "cpu/o3/vp/vp_history.hh"
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

// VpHistory (vp_history.hh) is the framework's fetch-side history
// register pair -- VtageTables folds pc/upc against its snapshotted
// {ghr, path}, but VpHistory itself is params-free and predictor-
// agnostic, so its own shift/fold/restore behavior is pinned here
// rather than only indirectly through VtageTables lookups.

// branchShift shifts the newest direction into bit 0; each later
// shift pushes older bits up one position.
TEST(VpHistory, GhrNewestBitOrdering)
{
    VpHistory h;
    h.branchShift(true);
    h.branchShift(false);
    // Newest (most recently shifted) direction is bit 0; the earlier
    // one has moved up to bit 1.
    EXPECT_EQ(h.state.ghr0 & 0x1ull, 0ull);
    EXPECT_EQ((h.state.ghr0 >> 1) & 0x1ull, 1ull);
    EXPECT_EQ(h.state.ghr0, 0x2ull);

    // A third shift moves both prior bits up one more position.
    h.branchShift(true);
    EXPECT_EQ(h.state.ghr0 & 0x1ull, 1ull);
    EXPECT_EQ((h.state.ghr0 >> 1) & 0x1ull, 0ull);
    EXPECT_EQ((h.state.ghr0 >> 2) & 0x1ull, 1ull);
    EXPECT_EQ(h.state.ghr0, 0x5ull);
}

// takenTarget folds low target-PC bits in at the bottom (3 bits per
// call: (target >> 2) & 7) and masks the result to pathBits wide.
// pathBits = 16 here is wide enough that the mask never truncates,
// so the exact fold recipe is checked directly; pathBits = 4 then
// reruns the identical two targets to confirm the mask does
// truncate the same computation's upper bits.
TEST(VpHistory, PathFoldAndMask)
{
    VpHistory h16;
    h16.takenTarget(0x400894, 16); // (0 << 3) | ((0x400894 >> 2) & 7)
    EXPECT_EQ(h16.state.path, 0x5u);
    h16.takenTarget(0x400898, 16); // (0x5 << 3) | ((0x400898 >> 2) & 7)
    EXPECT_EQ(h16.state.path, 0x2Eu);

    VpHistory h4;
    h4.takenTarget(0x400894, 4);
    EXPECT_EQ(h4.state.path, 0x5u); // Fits in 4 bits: no truncation yet.
    h4.takenTarget(0x400898, 4);
    // Unmasked value would be 0x2E (as above); masked to 4 bits it
    // truncates to 0x2E & 0xF == 0xE.
    EXPECT_EQ(h4.state.path, 0xEu);
}

// snapshotFor()/restoreHistory() (base.hh) are thin wrappers around
// exactly this copy/restore pair -- pin it directly at the
// VpHistory level: a snapshot taken mid-stream must be recoverable
// byte-for-byte after further advances.
TEST(VpHistory, SnapshotRestoreRoundtrip)
{
    VpHistory h;
    h.branchShift(true);
    h.takenTarget(0x400894, 16);
    h.branchShift(false);

    const VpHistSnapshot snap = h.state; // Copy at this point.

    h.branchShift(true);
    h.takenTarget(0x400898, 16);
    h.branchShift(true);
    ASSERT_NE(h.state.ghr0, snap.ghr0);
    ASSERT_NE(h.state.path, snap.path);

    h.restore(snap);
    EXPECT_EQ(h.state.ghr0, snap.ghr0);
    EXPECT_EQ(h.state.path, snap.path);
}

// The GHR is 128 bits across two 64-bit words: every branchShift must
// carry ghr0 bit 63 into ghr1 bit 0 (history bit 64). A single taken
// bit shifted up by 64 subsequent not-taken shifts must land exactly
// there -- read back through histBit(), the same accessor the
// VtageTables fold uses to span the word boundary.
TEST(VpHistory, GhrShiftCrossesWordBoundary)
{
    VpHistory h;
    h.branchShift(true); // History bit 0.
    for (int i = 0; i < 64; i++) {
        h.branchShift(false); // Each pushes it up one position.
    }
    // The taken bit is now history bit 64 == ghr1 bit 0; ghr0 holds
    // only the 64 not-taken bits shifted in after it.
    EXPECT_EQ(h.state.ghr0, 0ull);
    EXPECT_EQ(h.state.ghr1, 1ull);
    EXPECT_EQ(histBit(h.state, 64), 1ull);
    EXPECT_EQ(histBit(h.state, 63), 0ull);
    EXPECT_EQ(histBit(h.state, 65), 0ull);

    // One more shift moves it to history bit 65 (ghr1 bit 1) while
    // the newly shifted-in direction lands at bit 0.
    h.branchShift(true);
    EXPECT_EQ(histBit(h.state, 65), 1ull);
    EXPECT_EQ(histBit(h.state, 64), 0ull);
    EXPECT_EQ(histBit(h.state, 0), 1ull);
    EXPECT_EQ(h.state.ghr1, 2ull);
}

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
    VpHistSnapshot h{0x1234, 0, 7};
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
// Hand/model-verified: golden values hand-computed by replaying the
// FoldedHistory::update() recurrence.
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
    const VpHistSnapshot hBit3{h0.ghr0 ^ (1ull << 3), 0};
    EXPECT_EQ(t.lookup(pc, 0, hBit3).value, 222u);

    // Flip bit 0 (< every configured L): every tagged bank misses ->
    // falls all the way back to VT0.
    const VpHistSnapshot hBit0{h0.ghr0 ^ (1ull << 0), 0};
    EXPECT_EQ(t.lookup(pc, 0, hBit0).value, 111u);
}

// shiftedPcOf concatenates the micro-op residue (upc & 3) into the
// two low bits the >> 2 vacated -- that residue is what delivers
// micro-op separation into the hashed, masked tables (the high-bit
// vpKey fold alone lands in bits [48:63], which never survive the
// low masked bits any index/tag keeps). Two micro-ops of the same
// macro-op must get distinct entries and independently reach their
// own confidence holding their own values.
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

// Concatenating the micro-op residue must leave the PC-delta bits
// untouched. XORing the raw upc into the shifted PC instead (the
// pre-fix formula) toggled its low bits, so (pc, upc=1) aliased
// (pc+4, upc=0) for even pc>>2 (XOR 1 is +1 there) or (pc-4, upc=0)
// for odd pc>>2, in EVERY component's index AND tag (the bit-46 high
// fold never survives the masks) -- silently merging a cracked
// macro-op's second micro-op with a neighboring instruction. Under an
// equal, nonzero history each neighbor pair must resolve to distinct
// entries: distinct tokens once trained, and independently stored,
// independently retrievable values.
TEST(VtageTables, MicroOpDoesNotAliasNeighborPc)
{
    const Addr pcEven = 0x400890; // pcEven >> 2 is even.
    const Addr pcOdd = 0x400894;  // pcOdd >> 2 is odd.
    const VpHistSnapshot h{0x2b, 0, 0x13}; // Equal nonzero history.

    // Fresh table per pair: train key A to 111 and key B to 222 up
    // to confidence, then check full separation.
    auto checkDistinct = [&](Addr pcA, MicroPC upcA, Addr pcB,
                             MicroPC upcB) {
        VtageTables t(singleBankConfig(16, 2), constRng(0.0));

        // Wrong-train each key from its virgin VT0 entry: allocates
        // its own tagged (bank 1) entry.
        t.train(pcA, upcA, h, t.lookup(pcA, upcA, h).token, 111);
        t.train(pcB, upcB, h, t.lookup(pcB, upcB, h).token, 222);

        const auto tokA = t.lookup(pcA, upcA, h).token;
        const auto tokB = t.lookup(pcB, upcB, h).token;
        EXPECT_NE(tokA, tokB); // Distinct entries after training.

        t.train(pcA, upcA, h, tokA, 111); // Correct: c 0 -> 1.
        t.train(pcB, upcB, h, tokB, 222);
        t.train(pcA, upcA, h, tokA, 111); // Correct: c 1 -> 2.
        t.train(pcB, upcB, h, tokB, 222);

        // Independent value storage: an aliased pair would instead
        // ping-pong one shared entry between 111 and 222 and never
        // reach confidence.
        auto ra = t.lookup(pcA, upcA, h);
        auto rb = t.lookup(pcB, upcB, h);
        EXPECT_TRUE(ra.confident);
        EXPECT_TRUE(rb.confident);
        EXPECT_EQ(ra.value, 111u);
        EXPECT_EQ(rb.value, 222u);
    };

    checkDistinct(pcEven, 1, pcEven + 4, 0); // The even-pc>>2 alias.
    checkDistinct(pcEven, 1, pcEven - 4, 0);
    checkDistinct(pcOdd, 1, pcOdd - 4, 0); // The odd-pc>>2 alias.
    checkDistinct(pcOdd, 1, pcOdd + 4, 0);
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

        const VpHistSnapshot hFlip7{hBase.ghr0 ^ (1ull << 7), 0};
        EXPECT_NE(t.lookup(pc, 0, hFlip7).token, bank3Tok);

        const VpHistSnapshot hFlip8{hBase.ghr0 ^ (1ull << 8), 0};
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

        const VpHistSnapshot hFlip1{hBase.ghr0 ^ (1ull << 1), 0};
        EXPECT_NE(t.lookup(pc, 0, hFlip1).token, bank1Tok);
    }
}

// History bits beyond the first word must reach the fold: a component
// with L = 100 covers history bits 0..99, so flipping bit 99 (ghr1
// bit 35) must change that component's index/tag (a lookup that hit
// it now misses), while flipping bit 100 -- outside the window --
// must not.
TEST(VtageTables, LongHistoryBitsBeyond64ReachFold)
{
    VtageConfig c = comboConfig();
    c.historyLengths = {2, 4, 100};
    VtageTables t(c, constRng(0.9));
    const Addr pc = 0x1400;
    // History bit 99 set: the oldest bit of L = 100's window.
    const VpHistSnapshot hBase{0, 1ull << 35, 0};

    const auto virginTok = t.lookup(pc, 0, hBase).token;
    // 3 virgin candidates, r = 0.9 picks the last (bank 3, L = 100).
    t.train(pc, 0, hBase, virginTok, 777);
    const auto bank3Tok = t.lookup(pc, 0, hBase).token;
    EXPECT_NE(bank3Tok, virginTok);

    // Clear bit 99: inside the L = 100 window -> bank 3 misses (falls
    // back to VT0's own token).
    const VpHistSnapshot hFlip99{0, hBase.ghr1 ^ (1ull << 35), 0};
    EXPECT_NE(t.lookup(pc, 0, hFlip99).token, bank3Tok);

    // Set bit 100: outside the window -> bank 3 still hits.
    const VpHistSnapshot hFlip100{0, hBase.ghr1 ^ (1ull << 36), 0};
    EXPECT_EQ(t.lookup(pc, 0, hFlip100).token, bank3Tok);
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
    EXPECT_EQ(t.lookup(0x400004, 0,
                       VpHistSnapshot{0x123456789abcdef0ull, 0, 0x1234})
                  .token,
              65ull);
}

// The token's fixed 20-bit tag field must hold the widest legal tag:
// numTagged = 7 makes component 7's tag baseTagBits (12) + 7 = 19
// bits wide. Construct that geometry (taggedEntries = 256 keeps
// numTagged < log2(taggedEntries)), allocate into the topmost
// component under a full 128-bit history, and round-trip its token
// through every consumer: lookup restamps it, peekProvider decodes
// biased rank 8, train resolves it live (no StaleTokenRecomputed),
// correctiveReset tag-matches it.
TEST(VtageTables, Rank7TwentyBitTagTokenRoundTrips)
{
    VtageConfig c;
    c.baseEntries = 16;
    c.taggedEntries = 256;
    c.numTagged = 7;
    c.historyLengths = {2, 5, 11, 26, 58, 90, 128};
    c.baseTagBits = 12;
    c.confBits = 3;
    c.confThreshold = 7;
    c.fpcVector = {1, 1. / 16, 1. / 16, 1. / 16, 1. / 16, 1. / 32, 1. / 32};
    c.pathBits = 16;
    VtageTables t(c, constRng(0.99));

    const Addr pc = 0x400890;
    const VpHistSnapshot h{0xdeadbeefcafef00dull, 0x123456789abcdef0ull,
                           0x1a2b};
    const auto virginTok = t.lookup(pc, 0, h).token;
    // 7 virgin candidates, r = 0.99 picks the last (bank 7, L = 128).
    t.train(pc, 0, h, virginTok, 777);

    const auto tok = t.lookup(pc, 0, h).token;
    EXPECT_NE(tok, virginTok);
    EXPECT_EQ(t.lookup(pc, 0, h).value, 777u);
    EXPECT_EQ(t.peekProvider(pc, 0, h, tok).rank, 8u); // Biased VT7.

    // A live-token train: no StaleTokenRecomputed -- the unpacked
    // {rank, index, tag} matched the allocated entry exactly.
    EXPECT_EQ(t.train(pc, 0, h, tok, 777),
              Outcomes{VtageTrainOutcome::CorrectInc});
    EXPECT_TRUE(t.correctiveReset(tok));
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

    const VpHistSnapshot hBit2{h0.ghr0 ^ (1ull << 2), 0};
    EXPECT_EQ(t.lookup(pc, 0, hBit2).token, virginTok); // Falls to VT0.

    const VpHistSnapshot hBit4{h0.ghr0 ^ (1ull << 4), 0};
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
    const VpHistSnapshot hBit3{h0.ghr0 ^ (1ull << 3), 0};
    const auto bank1Tok = t.lookup(pc, 0, hBit3).token;
    ASSERT_EQ(t.lookup(pc, 0, hBit3).value, 222u);

    // Correct-train bank 1 in place: u -> 1.
    EXPECT_EQ(t.train(pc, 0, hBit3, bank1Tok, 222),
              Outcomes{VtageTrainOutcome::CorrectInc});

    // A different (pc, ghr) pair whose own bank-1 probe index
    // coincides with bank1Tok's cell (hand-derived collision under
    // the concatenating shiftedPcOf), but whose tag differs, so its
    // own lookup falls straight to VT0.
    const Addr pc2 = 1040;
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

    // Self-check that the collision really happened (guards against
    // this test going vacuous if the index arithmetic shifts): the
    // rng pick (0.0 = first qualifying candidate) can only have
    // skipped bank 1 -- landing pc2's entry in bank 2 -- because
    // bank 1's candidate WAS bank1Tok's u == 1 cell. A bank-2 entry
    // vanishes when ghr bit 3 flips (inside L = 4's window, outside
    // L = 2's), demoting pc2 back to its VT0 token; a (wrongly)
    // allocated bank-1 entry would survive that flip and still hit.
    const VpHistSnapshot hProbeBit3{hProbe.ghr0 ^ (1ull << 3), 0};
    EXPECT_EQ(t.lookup(pc2, 0, hProbeBit3).token, probeVirginTok);
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
