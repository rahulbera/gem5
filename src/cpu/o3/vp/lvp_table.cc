#include "cpu/o3/vp/lvp_table.hh"

#include <algorithm>

#include "base/intmath.hh"
#include "base/logging.hh"

namespace gem5
{
namespace o3
{

LvpTable::LvpTable(const LvpConfig &cfg)
    : sets(cfg.assoc ? cfg.entries / cfg.assoc : 0),
      assoc(cfg.assoc),
      confMax(cfg.confBits >= 32 ? ~0u : (1u << cfg.confBits) - 1),
      confThreshold(cfg.confThreshold),
      confDecrementOnWrong(cfg.confDecrementOnWrong),
      table(sets * cfg.assoc)
{
    fatal_if(cfg.assoc == 0 || cfg.entries % cfg.assoc != 0,
             "LVP entries (%u) must be a nonzero multiple of assoc (%u)",
             cfg.entries, cfg.assoc);
    fatal_if(cfg.confBits == 0 || cfg.confBits > 16,
             "LVP confBits (%u) must be in [1, 16]", cfg.confBits);
    fatal_if(!isPowerOf2(sets), "LVP set count (%u) must be a power of two",
             sets);
    fatal_if(confThreshold > confMax,
             "LVP confThreshold (%u) exceeds the %u-bit counter max (%u)",
             confThreshold, cfg.confBits, confMax);
    // Threshold 0 makes conf >= confThreshold a tautology: a punished
    // entry re-predicts immediately, voiding the no-livelock
    // precondition that a wrong verify leaves confidence below the
    // threshold.
    fatal_if(confThreshold == 0,
             "LVP confThreshold must be >= 1: 0 predicts on every table "
             "hit, so a wrong verify cannot leave the entry below "
             "threshold (squash-livelock hazard)");
}

unsigned
LvpTable::setIndex(Addr key) const
{
    // Low two bits of an AArch64 PC are zero; the micro-PC is folded into
    // the high bits (vp_key.hh), so bits [2..] index well.
    return (key >> 2) & (sets - 1);
}

LvpLookup
LvpTable::lookup(Addr key) const
{
    const Entry *set = &table[setIndex(key) * assoc];
    for (unsigned w = 0; w < assoc; w++) {
        if (set[w].valid && set[w].tag == key) {
            LvpLookup r;
            r.hit = true;
            r.confident = set[w].conf >= confThreshold;
            r.value = set[w].lastValue;
            return r;
        }
    }
    return {};
}

LvpTrainOutcome
LvpTable::train(Addr key, RegVal actual)
{
    Entry *set = &table[setIndex(key) * assoc];
    Entry *hit = nullptr;
    for (unsigned w = 0; w < assoc; w++) {
        if (set[w].valid && set[w].tag == key) {
            hit = &set[w];
            break;
        }
    }

    if (!hit) {
        Entry *victim = &set[0];
        for (unsigned w = 1; w < assoc && victim->valid; w++) {
            if (!set[w].valid || set[w].lastUse < victim->lastUse) {
                victim = &set[w];
            }
        }
        victim->valid = true;
        victim->tag = key;
        victim->lastValue = actual;
        victim->conf = 0;
        victim->lastUse = ++useCounter;
        return LvpTrainOutcome::Allocated;
    }

    hit->lastUse = ++useCounter;
    if (actual == hit->lastValue) {
        if (hit->conf < confMax) {
            hit->conf++;
        }
        return LvpTrainOutcome::Match;
    }

    hit->lastValue = actual;
    if (confDecrementOnWrong) {
        // No-livelock invariant: a wrong verify must leave confidence
        // BELOW the predict threshold, so the refetched instance is not
        // re-predicted. A plain decrement violates that whenever
        // conf > confThreshold; clamp to confThreshold - 1 (the
        // constructor guarantees confThreshold >= 1).
        if (hit->conf > 0) {
            hit->conf = std::min(hit->conf - 1, confThreshold - 1);
        }
        return LvpTrainOutcome::MismatchDecrement;
    }
    hit->conf = 0;
    return LvpTrainOutcome::MismatchReset;
}

} // namespace o3
} // namespace gem5
