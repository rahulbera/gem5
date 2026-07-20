#ifndef __CPU_O3_MEM_RENAME_PREDICTOR_HH__
#define __CPU_O3_MEM_RENAME_PREDICTOR_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/o3/mrn_squash_reason.hh"
#include "sim/sim_object.hh"

namespace gem5
{

// Forward declaration of the generated params struct. Keeping the SimObject
// constructor out-of-line (in mem_rename_predictor_sim.cc) lets the unit test
// exercise the plain-int core (MrnTables) without pulling in the generated
// params/MemRenamePredictor.hh header.
struct MemRenamePredictorParams;

namespace o3
{

/**
 * Result of a memory-renaming value prediction for a load. A high-confidence
 * hit returns valid=true with the snapshotted producer value.
 */
struct MrnPrediction
{
    bool valid;
    RegVal value;
};

/**
 * Plain-int configuration for MrnTables. Mirrors the SimObject params but is
 * free of any params/SimObject machinery, so the core logic can be built and
 * unit-tested in isolation.
 */
struct MrnConfig
{
    unsigned storeTableEntries = 1024;
    unsigned storeTableAssoc = 4;
    unsigned loadTableEntries = 1024;
    unsigned loadTableAssoc = 4;
    unsigned valueFileEntries = 512;
    unsigned confBits = 4;
    unsigned confThreshold = 8;
    unsigned confInc = 1;
    unsigned confDec = 1;
    bool resetConfOnMispredict = true;
};

/**
 * Core of the memory-renaming predictor (Garfield Stage 2, mode B = value
 * snapshot). Owns the three structures and all predict/train logic, and is
 * constructed purely from plain ints (MrnConfig) so it is unit-testable
 * without the SimObject/params machinery. MemRenamePredictor is a thin
 * SimObject wrapper that delegates to it.
 *
 *  - Store cache: effAddr -> value-file slot index. Set-associative with
 *                 modulo indexing and LRU replacement within a set.
 *  - Value file:  an independent vector of {value, valid} slots with LRU.
 *                 Kept separate from the store cache because a later mode (C)
 *                 repurposes these slots.
 *  - Load cache:  loadPC -> {value-file slot index, confidence}. Also
 *                 set-associative with modulo indexing and LRU.
 */
class MrnTables
{
  public:
    explicit MrnTables(const MrnConfig &cfg);

    /** Rename: return a high-confidence value prediction for a load PC. */
    MrnPrediction predict(Addr loadPC);

    /** Commit: a retiring store deposits its value for an address. */
    void commitStore(Addr effAddr, RegVal value);

    /** Commit: bind a load PC to the producer slot and train confidence. */
    void commitLoad(Addr loadPC, Addr effAddr, RegVal realValue, bool isSpGp);

    /** Writeback: reset a load PC's confidence (loop-safety). */
    void mispredict(Addr loadPC);

    /** Correlator (mode C, C.1 lsq_forward): record that a load PC forwarded
     *  from a store PC (learned at the LSQ store->load forward event). */
    void trainForward(Addr loadPC, Addr storePC);

    /** Correlator: the store PC most recently bound to this load PC, or 0 if
     *  none is known. Used at rename to find an in-flight producing store. */
    Addr predictProducerPC(Addr loadPC);

  private:
    struct StoreEntry
    {
        Addr tag = 0;
        int slot = -1;
        bool valid = false;
        uint64_t lru = 0;
    };

    struct LoadEntry
    {
        Addr tag = 0;
        int slot = -1;
        unsigned conf = 0;
        bool valid = false;
        uint64_t lru = 0;
    };

    struct ValueSlot
    {
        RegVal value = 0;
        bool valid = false;
        uint64_t lru = 0;
    };

    /** Correlator entry (C.1): loadPC -> producing storePC. Set-associative,
     *  sized and indexed exactly like the load cache. */
    struct FwdEntry
    {
        Addr tag = 0;
        Addr storePC = 0;
        bool valid = false;
        uint64_t lru = 0;
    };

    /** Find the store-cache entry for addr, or nullptr on a miss. */
    StoreEntry *storeFind(Addr addr);
    /** Allocate (LRU-evict within the set) a store-cache entry for addr. */
    StoreEntry *storeAllocate(Addr addr);
    /** Find the load-cache entry for loadPC, or nullptr on a miss. */
    LoadEntry *loadFind(Addr loadPC);
    /** Allocate (LRU-evict within the set) a load-cache entry for loadPC. */
    LoadEntry *loadAllocate(Addr loadPC);
    /** Allocate (LRU-evict) a value-file slot and return its index. */
    int valueAllocate();
    /** Find the correlator entry for loadPC, or nullptr on a miss. */
    FwdEntry *fwdFind(Addr loadPC);
    /** Allocate (LRU-evict within the set) a correlator entry for loadPC. */
    FwdEntry *fwdAllocate(Addr loadPC);

    const unsigned storeSets;
    const unsigned storeAssoc;
    const unsigned loadSets;
    const unsigned loadAssoc;
    const unsigned confThreshold;
    const unsigned confInc;
    const unsigned confDec;
    const unsigned confMax;
    const bool resetConfOnMispredict;

    std::vector<StoreEntry> storeCache;
    std::vector<LoadEntry> loadCache;
    std::vector<ValueSlot> valueFile;
    /** Correlator table (C.1), sized/indexed like the load cache. */
    std::vector<FwdEntry> fwdCache;

    /** Monotonic counter used as the LRU timestamp for all structures. */
    uint64_t lruTick = 0;
};

/**
 * Memory-renaming predictor SimObject. A thin wrapper that owns an MrnTables
 * and forwards each public operation to it; all behaviour lives in MrnTables
 * so it can be unit-tested without the params/SimObject machinery.
 */
class MemRenamePredictor : public SimObject
{
  public:
    MemRenamePredictor(const MemRenamePredictorParams &p);

    /** Rename: high-confidence value prediction for a load PC. */
    MrnPrediction
    predict(Addr loadPC)
    {
        stats.predictLookups++;
        return tables.predict(loadPC);
    }

    /** Commit: a retiring store deposits a value. */
    void
    commitStore(Addr effAddr, RegVal value)
    {
        stats.storesTrained++;
        tables.commitStore(effAddr, value);
    }

    /** Commit: bind the load to the producer slot and train confidence. */
    void
    commitLoad(Addr loadPC, Addr effAddr, RegVal realValue, bool isSpGp)
    {
        stats.loadsTrained++;
        tables.commitLoad(loadPC, effAddr, realValue, isSpGp);
    }

    /** Writeback: a forwarded load mispredicted. Reset confidence
     *  (loop-safety) and record the flush cost (squashedInsts = the load plus
     *  every younger in-flight instruction, all discarded by the squash). */
    void
    mispredict(Addr loadPC, uint64_t squashedInsts)
    {
        stats.mispredicts++;
        stats.squashedInsts += squashedInsts;
        tables.mispredict(loadPC);
    }

    /** Rename: a prediction was actually forwarded into a renamed reg. */
    void
    noteForwarded()
    {
        stats.predictionsMade++;
    }

    /** Writeback: a forwarded load verified correct (mode B value path). */
    void
    noteCorrect()
    {
        stats.predictionsCorrect++;
    }

    /** Rename (mode C): record loadPC->storePC, learned from the LSQ
     *  store->load forward event. */
    void
    trainForward(Addr loadPC, Addr storePC)
    {
        stats.bindingsLearned++;
        tables.trainForward(loadPC, storePC);
    }

    /** Rename (mode C): the store PC bound to this load PC, or 0 if none.
     *  store_set correlation is a stub and always returns 0 (no producer). */
    Addr
    predictProducerPC(Addr loadPC)
    {
        if (_useStoreSet) {
            return 0;
        }
        return tables.predictProducerPC(loadPC);
    }

    /** Rename: a load was forwarded via the mode-B value snapshot. */
    void
    noteForwardValue()
    {
        stats.forwardsValue++;
    }

    /** Rename: a load was aliased to an in-flight producer's physreg (C). */
    void
    noteForwardAlias()
    {
        stats.forwardsAlias++;
    }

    /** Writeback: an aliased load verified correct. */
    void
    noteAliasCorrect()
    {
        stats.aliasVerifyCorrect++;
    }

    /** Rename (diagnostic): whether the mode-C alias resolved to a physreg
     *  different from the one the located store captured -- i.e. the data
     *  arch reg was redefined in between, so the alias bets on register
     *  liveness rather than memory dataflow. */
    void
    noteAliasProducerStaleness(bool stale)
    {
        if (stale) {
            stats.aliasProducerStale++;
        } else {
            stats.aliasProducerCurrent++;
        }
    }

    /** Verify (diagnostic): the alias outcome bucketed by that staleness.
     *  Index order matches aliasStaleOutcomeNames. */
    void
    noteAliasOutcomeByStaleness(bool stale, bool correct)
    {
        stats.aliasOutcomeByStaleness[(stale ? 2 : 0) + (correct ? 1 : 0)]++;
    }

    /** Writeback: an aliased load's producer was not yet ready, so the
     *  verification was deferred until the producer wrote back. */
    void
    noteAliasWaited()
    {
        stats.aliasVerifyWaitedForProducer++;
    }

    /** Squash: an MRN-forwarded load was discarded before it could verify.
     *  Called once per such load from ROB::doSquash. @param is_alias true if
     *  the load took the mode-C alias path, false for the mode-B value path;
     *  @param reason why the squash was raised. Together with
     *  predictionsCorrect/mispredicts this closes the per-path accounting:
     *    forwardsValue == predictionsCorrect + mispredicts
     *                     + predictionsSquashedValue::total
     *    forwardsAlias == aliasVerifyCorrect + aliasMispredicts
     *                     + predictionsSquashedAlias::total
     *  (to within the MRN loads still in flight at the dump boundary). */
    void
    noteSquashedPrediction(bool is_alias, MrnSquashReason reason)
    {
        const int idx = static_cast<int>(reason);
        if (is_alias) {
            stats.predictionsSquashedAlias[idx]++;
        } else {
            stats.predictionsSquashedValue[idx]++;
        }
    }

    /** Verify: an aliased load mispredicted; reset confidence and record the
     *  flush cost (the load plus every younger in-flight inst). */
    void
    aliasMispredict(Addr loadPC, uint64_t squashedInsts)
    {
        stats.aliasMispredicts++;
        stats.squashedInsts += squashedInsts;
        tables.mispredict(loadPC);
    }

    /** Whether MRN forwarding is restricted to integer-destination loads. */
    bool
    predictIntLoadsOnly() const
    {
        return _predictIntLoadsOnly;
    }

    /** Mode C: reject an alias whose rename-map lookup no longer matches the
     *  physreg the located store captured. See the param description. */
    bool
    aliasRequireCurrentProducer() const
    {
        return _aliasRequireCurrentProducer;
    }

    /** Whether mode C (producer aliasing) is enabled (else value_only/B). */
    bool
    unified() const
    {
        return _unified;
    }

  private:
    MrnTables tables;
    const bool _aliasRequireCurrentProducer;

    const bool _predictIntLoadsOnly;
    /** mrnMode == unified (mode C active, subsumes B). */
    const bool _unified;
    /** mrnCorrelation == store_set (stub: predictProducerPC returns 0). */
    const bool _useStoreSet;

    /** Training statistics (Garfield Stage 2, MRN). */
    struct MemRenameStats : public statistics::Group
    {
        explicit MemRenameStats(statistics::Group *parent);
        /** Stores that deposited a value into the MRN value file. */
        statistics::Scalar storesTrained;
        /** Loads that trained the MRN predictor at commit. */
        statistics::Scalar loadsTrained;
        /** Loads looked up in the predictor at rename (coverage base). */
        statistics::Scalar predictLookups;
        /** High-confidence predictions forwarded into a renamed reg. */
        statistics::Scalar predictionsMade;
        /** Forwarded loads that verified correct at writeback. */
        statistics::Scalar predictionsCorrect;
        /** Forwarded loads that verified wrong and forced a squash. */
        statistics::Scalar mispredicts;
        /** Instructions discarded by MRN misprediction squashes (the load
         *  plus all younger in-flight insts): the recovery cost / wasted
         *  work of the pipe flush. Summed over both the value and alias
         *  paths. */
        statistics::Scalar squashedInsts;
        /** High-confidence loads forwarded via the mode-B value snapshot. */
        statistics::Scalar forwardsValue;
        /** High-confidence loads aliased to an in-flight producer's physreg
         *  (mode C). */
        statistics::Scalar forwardsAlias;
        /** Aliased loads that verified correct at writeback. */
        statistics::Scalar aliasVerifyCorrect;
        /** Aliased loads that verified wrong and forced a squash. */
        statistics::Scalar aliasMispredicts;
        /** Aliased loads whose producer was not yet ready at the load's
         *  writeback, so verification waited for the producer (the mode-C
         *  ingenuity: the alias bought work the value path could not). */
        statistics::Scalar aliasVerifyWaitedForProducer;
        /** loadPC->storePC correlator bindings learned from LSQ forwarding. */
        statistics::Scalar bindingsLearned;

        /** Mode-B (value) forwards discarded before they could verify,
         *  indexed by MrnSquashReason. */
        statistics::Vector predictionsSquashedValue;

        /** Mode-C (alias) forwards discarded before they could verify,
         *  indexed by MrnSquashReason. */
        statistics::Vector predictionsSquashedAlias;

        /** Diagnostic: alias attempts whose rename-map lookup matched the
         *  located store's own captured data physreg. */
        statistics::Scalar aliasProducerCurrent;

        /** Diagnostic: alias attempts where it did NOT match (the data arch
         *  reg was redefined between the store's rename and the load's). */
        statistics::Scalar aliasProducerStale;

        /** Diagnostic: alias verify outcome bucketed by staleness --
         *  {currentWrong, currentCorrect, staleWrong, staleCorrect}. */
        statistics::Vector aliasOutcomeByStaleness;

    } stats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MEM_RENAME_PREDICTOR_HH__
