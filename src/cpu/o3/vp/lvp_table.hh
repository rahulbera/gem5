#ifndef __CPU_O3_VP_LVP_TABLE_HH__
#define __CPU_O3_VP_LVP_TABLE_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{
namespace o3
{

/**
 * Plain-int configuration for LvpTable. Mirrors the LastValueVP SimObject
 * params but is free of any params/SimObject machinery, so the core logic
 * can be built and unit-tested in isolation (the pattern proven by
 * MrnValueFileTables).
 */
struct LvpConfig
{
    unsigned entries = 4096;
    unsigned assoc = 4;
    /** Saturating confidence counter width. */
    unsigned confBits = 4;
    /** Minimum confidence required to predict. Must be >= 1: 0 would
     *  predict on every table hit, voiding the no-livelock invariant
     *  (the constructor rejects it). */
    unsigned confThreshold = 15;
    /** Decrement (rather than reset to zero) confidence on a value
     *  mismatch, clamped below confThreshold so a wrong verify always
     *  leaves the entry unable to predict. */
    bool confDecrementOnWrong = false;
};

/** What a rename-time lookup saw. */
struct LvpLookup
{
    bool hit = false;
    /** conf >= confThreshold: the value below may be consumed. */
    bool confident = false;
    RegVal value = 0;
};

/** What a train() call did (feeds the wrapper's derived stats). */
enum class LvpTrainOutcome
{
    /** Miss filled an invalid way. */
    Allocated,
    /** Miss displaced a valid entry (the table-thrash signal). */
    Evicted,
    Match,
    MismatchReset,
    MismatchDecrement
};

/**
 * The last-value prediction table (VPT): set-associative, LRU, full tags
 * (a research simulator should not fold false aliasing artifacts into
 * results). Entry: {valid, tag, lastValue, conf}.
 */
class LvpTable
{
  public:
    explicit LvpTable(const LvpConfig &cfg);

    /** Rename-time lookup. Never modifies the table -- recency is
     *  tracked at train, which runs for every in-scope instruction. */
    LvpLookup lookup(Addr key) const;

    /** Writeback-time training with the architected value: match
     *  increments confidence (saturating); mismatch resets it (default)
     *  or decrements it clamped below confThreshold (so a wrong verify
     *  always lands below the predict threshold -- the no-livelock
     *  invariant), and always updates the stored value; a miss
     *  allocates over the LRU way with confidence zero. */
    LvpTrainOutcome train(Addr key, RegVal actual);

  private:
    struct Entry
    {
        bool valid = false;
        Addr tag = 0;
        RegVal lastValue = 0;
        unsigned conf = 0;
        uint64_t lastUse = 0;
    };

    unsigned setIndex(Addr key) const;

    const unsigned sets;
    const unsigned assoc;
    const unsigned confMax;
    const unsigned confThreshold;
    const bool confDecrementOnWrong;

    /** Monotonic use counter backing LRU (no wall-clock dependence). */
    uint64_t useCounter = 0;

    /** sets x assoc entries, row-major by set. */
    std::vector<Entry> table;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_VP_LVP_TABLE_HH__
