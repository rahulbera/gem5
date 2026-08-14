#include "cpu/o3/vp/estride_table.hh"

#include <cmath>
#include <cstdlib>
#include <utility>

#include "base/logging.hh"

namespace gem5
{
namespace o3
{

EStrideTable::EStrideTable(std::function<double()> rng) : rng(std::move(rng))
{}

unsigned
EStrideTable::wayIndex(uint64_t key, unsigned way)
{
    return ((key ^ (key >> (2 * LogSets - way)) ^ (key >> (LogSets - way)) ^
             (key >> (3 * LogSets - way))) *
                NumWays +
            way) %
           NumEntries;
}

uint64_t
EStrideTable::wayTag(uint64_t key, unsigned way)
{
    // j = NumWays - way is always in {1, 2, 3} for way in {0, 1, 2};
    // the source's dead `if (j < 0) j = 0` clamp (cc:77-78) is
    // unreachable and dropped.
    const unsigned j = NumWays - way;
    return ((key >> (LogSets - j)) ^ (key >> (2 * LogSets - j)) ^
            (key >> (3 * LogSets - j)) ^ (key >> (4 * LogSets - j))) &
           ((1u << TagBits) - 1);
}

int
EStrideTable::findEntry(uint64_t key) const
{
    for (unsigned way = 0; way < NumWays; way++) {
        const unsigned idx = wayIndex(key, way);
        if (table[idx].tag == wayTag(key, way)) {
            return static_cast<int>(idx);
        }
    }
    return -1;
}

EStrideLookup
EStrideTable::lookup(uint64_t key, unsigned inflight) const
{
    EStrideLookup out;
    const int idx = findEntry(key);
    if (idx < 0) {
        return out;
    }
    const Entry &e = table[idx];
    out.hit = true;
    const bool wouldPredict = e.conf >= ConfThreshold;
    out.predicted = safeStride_ >= 0 && wouldPredict;
    out.blockedBySafeStride = safeStride_ < 0 && wouldPredict;
    if (out.predicted) {
        out.value =
            static_cast<uint64_t>(static_cast<int64_t>(e.lastValue) +
                                  (static_cast<int64_t>(inflight) + 1) *
                                      static_cast<int64_t>(e.stride));
    }
    return out;
}

bool
EStrideTable::bernoulli(unsigned exponent)
{
    // p = 2^-exponent (exponent 0 always passes; rng() in [0, 1)).
    return rng() < std::ldexp(1.0, -static_cast<int>(exponent));
}

bool
EStrideTable::confIncrementDraw(const EStrideClassifier &c, int64_t stride)
{
    // Deterministic conjunct short-circuits ahead of every draw: a
    // correct prediction covered by VTAGE alone (delivered-correct
    // without the stride flag) must not warm the stride entry.
    if (c.deliveredCorrect && !c.stridePredicted) {
        return false;
    }
    const unsigned exponent = c.notLlcMiss + c.notL2Miss + c.notL1Miss +
                              2 * c.fastInst + 2 * !c.isLoad;
    // 1 draw; doubled at stride >= 8 and again at stride >= 64 --
    // SIGNED comparisons (the source's abs-of-a-boolean quirk,
    // cc:154-155): negative strides never get the extra draws.
    unsigned draws = 1;
    if (stride >= 8) {
        draws *= 2;
    }
    if (stride >= 64) {
        draws *= 2;
    }
    bool passed = false;
    for (unsigned i = 0; i < draws && !passed; i++) {
        passed = bernoulli(exponent);
    }
    // Small-stride load throttle (cc:156-159): stride-0 loads never
    // pass; -1 draws an extra 1/2 coin, +1 an extra 1/4 coin. The
    // coin is drawn only when its stride case applies (clean draw
    // discipline; probability-equivalent to the source's
    // both-sides-evaluated bitwise &).
    bool filter = std::llabs(stride) > 1 || !c.isLoad;
    if (!filter && stride == -1) {
        filter = bernoulli(1);
    }
    if (!filter && stride == 1) {
        filter = bernoulli(2);
    }
    return passed && filter;
}

bool
EStrideTable::allocationDraw(const EStrideClassifier &c)
{
    switch (c.allocClass) {
        case EStrideAllocClass::AluOrStore:
            return bernoulli(6);
        case EStrideAllocClass::FpOrSlowAlu:
            return bernoulli(4);
        case EStrideAllocClass::Load:
            return bernoulli(c.notLlcMiss + c.notL2Miss + c.notL1Miss +
                             c.fastInst);
        case EStrideAllocClass::Never:
        default:
            return false; // No draw consumed.
    }
}

void
EStrideTable::installEntry(unsigned idx, uint64_t key, unsigned way,
                           uint64_t actual)
{
    Entry &e = table[idx];
    // conf = 1 deliberately, to survive victim pass 1 until the first
    // stride observation (cc:320).
    e.conf = 1;
    e.u = 0;
    e.tag = static_cast<uint16_t>(wayTag(key, way));
    e.stride = 0;
    e.notFirstOcc = false;
    e.lastValue = actual;
}

void
EStrideTable::allocateVictim(uint64_t key, uint64_t actual,
                             std::vector<EStrideTrainOutcome> &outcomes)
{
    const unsigned startWay = static_cast<unsigned>(rng() * NumWays);
    // Pass 1: claim the first way (from startWay) with conf == 0.
    for (unsigned i = 0; i < NumWays; i++) {
        const unsigned way = (startWay + i) % NumWays;
        const unsigned idx = wayIndex(key, way);
        if (table[idx].conf == 0) {
            installEntry(idx, key, way, actual);
            outcomes.push_back(EStrideTrainOutcome::AllocatedConfZeroVictim);
            return;
        }
    }
    // Pass 2: claim the first way (from startWay) with u == 0.
    unsigned lastIdx = 0;
    for (unsigned i = 0; i < NumWays; i++) {
        const unsigned way = (startWay + i) % NumWays;
        const unsigned idx = wayIndex(key, way);
        lastIdx = idx;
        if (table[idx].u == 0) {
            installEntry(idx, key, way, actual);
            outcomes.push_back(EStrideTrainOutcome::AllocatedUZeroVictim);
            return;
        }
    }
    // Both passes failed: age the last-probed way's entry. Pass 2
    // would have claimed any u == 0 entry, so u > 0 here by
    // construction.
    Entry &e = table[lastIdx];
    gem5_assert(e.u > 0, "E-Stride aging a u == 0 entry: victim pass 2 should "
                         "have claimed it first");
    const unsigned exponent = 2 + 2 * (e.conf > ConfMax / 8 ? 1u : 0u) +
                              2 * (e.conf >= ConfThreshold ? 1u : 0u);
    if (bernoulli(exponent)) {
        e.u--;
        outcomes.push_back(EStrideTrainOutcome::AllocAged);
    } else {
        outcomes.push_back(EStrideTrainOutcome::AllocAgeHeld);
    }
}

std::vector<EStrideTrainOutcome>
EStrideTable::train(uint64_t key, RegVal actual, const EStrideClassifier &c)
{
    std::vector<EStrideTrainOutcome> outcomes;

    // SafeStride tick: +1 per in-scope committed instruction,
    // pre-checked against the cap (cc:809-810).
    if (safeStride_ < SafeStrideCap) {
        safeStride_++;
    }
    // SafeStride credit: +4 (+8 for loads) when this instruction's
    // delivered prediction was correct AND supplied by this table,
    // pre-check-then-add (can legally overshoot the cap, cc:815-817).
    if (c.deliveredCorrect && c.stridePredicted &&
        safeStride_ < SafeStrideCap) {
        safeStride_ += 4 * (1 + (c.isLoad ? 1 : 0));
        outcomes.push_back(EStrideTrainOutcome::SafeStrideCredited);
    }

    const int idx = findEntry(key);
    if (idx < 0) {
        // Tag miss: allocation is attempted only when this
        // instruction was not already delivered correct (by VTAGE) --
        // don't waste a stride entry on it.
        if (c.deliveredCorrect) {
            outcomes.push_back(
                EStrideTrainOutcome::AllocSkippedDeliveredCorrect);
        } else if (!allocationDraw(c)) {
            outcomes.push_back(EStrideTrainOutcome::AllocDrawRefused);
        } else {
            allocateVictim(key, actual, outcomes);
        }
        return outcomes;
    }

    Entry &e = table[idx];
    const uint64_t lastValueOld = e.lastValue;
    const uint64_t oneStep = static_cast<uint64_t>(
        static_cast<int64_t>(lastValueOld) + static_cast<int64_t>(e.stride));
    const int64_t delta =
        static_cast<int64_t>(actual) - static_cast<int64_t>(lastValueOld);
    // Overflow-free interval test for the source's asymmetric window
    // (design doc deviation 8): delta in [-2^19 + 1, +2^19].
    const bool inRange =
        delta >= -(int64_t{1} << 19) + 1 && delta <= (int64_t{1} << 19);
    const uint64_t strideCandidate =
        inRange ? static_cast<uint64_t>(delta) : 0;
    e.lastValue = actual; // Unconditional, before any branch (cc:241).

    if (e.notFirstOcc && oneStep == actual) {
        // Trained, one-step match: the raw arithmetic identity above
        // (never a comparison against strideCandidate).
        if (e.conf < ConfMax) {
            if (confIncrementDraw(c, static_cast<int64_t>(strideCandidate))) {
                e.conf++;
                outcomes.push_back(EStrideTrainOutcome::ConfInc);
            } else {
                outcomes.push_back(EStrideTrainOutcome::ConfHeld);
            }
        }
        if (e.u < 3) {
            // Independent draw from the conf one (cc:249-259).
            if (confIncrementDraw(c, static_cast<int64_t>(strideCandidate))) {
                e.u++;
                outcomes.push_back(EStrideTrainOutcome::UInc);
            } else {
                outcomes.push_back(EStrideTrainOutcome::UHeld);
            }
        }
        if (e.conf >= ConfThreshold && e.u != 3) {
            e.u = 3;
            outcomes.push_back(EStrideTrainOutcome::UJamSaturated);
        } else if (e.conf >= ConfThreshold) {
            e.u = 3; // Write unconditional, as in cc:260-261.
        }
    } else if (e.notFirstOcc) {
        // Trained, one-step mismatch: table-local, fires regardless
        // of whether a pipeline prediction was issued. Stride
        // untouched.
        if (e.conf > ConfDecay) {
            e.conf -= ConfDecay;
            outcomes.push_back(EStrideTrainOutcome::MispredictDecay);
        } else {
            e.conf = 0;
            e.u = 0;
            outcomes.push_back(EStrideTrainOutcome::MispredictCollapse);
        }
        e.notFirstOcc = false;
    } else {
        // First occurrence.
        if (strideCandidate != 0) {
            e.stride = strideCandidate;
            outcomes.push_back(EStrideTrainOutcome::StrideSet);
        } else {
            e.stride = StrideSentinel;
            e.conf = 0;
            e.u = 0;
            outcomes.push_back(EStrideTrainOutcome::SentinelDemoted);
        }
        e.notFirstOcc = true;
    }

    return outcomes;
}

bool
EStrideTable::safeStridePenalty()
{
    const bool was_non_negative = safeStride_ >= 0;
    safeStride_ -= 1024;
    return was_non_negative && safeStride_ < 0;
}

int
EStrideTable::safeStride() const
{
    return safeStride_;
}

EStridePeek
EStrideTable::peek(uint64_t key) const
{
    const int idx = findEntry(key);
    if (idx < 0) {
        return EStridePeek{};
    }
    const Entry &e = table[idx];
    return EStridePeek{true,          e.conf,   e.u,
                       e.notFirstOcc, e.stride, e.lastValue};
}

void
EStrideTable::setSafeStrideForTest(int v)
{
    safeStride_ = v;
}

} // namespace o3
} // namespace gem5
