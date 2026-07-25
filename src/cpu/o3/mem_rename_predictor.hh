#ifndef __CPU_O3_MEM_RENAME_PREDICTOR_HH__
#define __CPU_O3_MEM_RENAME_PREDICTOR_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/o3/mem_rename_valuefile.hh"
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
 * Core of the memory-renaming predictor (Garfield MRN, value-snapshot
 * forwarding). Owns the three structures and all predict/train logic, and is
 * constructed purely from plain ints (MrnConfig) so it is unit-testable
 * without the SimObject/params machinery. MemRenamePredictor is a thin
 * SimObject wrapper that delegates to it.
 *
 *  - Store cache: effAddr -> value-file slot index. Set-associative with
 *                 modulo indexing and LRU replacement within a set.
 *  - Value file:  an independent vector of {value, valid} slots with LRU.
 *                 Kept separate from the store cache because the
 *                 producer-aliasing path repurposes these slots.
 *  - Load cache:  loadPC -> {value-file slot index, confidence}. Also
 *                 set-associative with modulo indexing and LRU.
 */
class MrnTables
{
  public:
    explicit MrnTables(const MrnConfig &cfg);

    /** Rename: return a high-confidence value prediction for a load PC. */
    MrnPrediction predict(Addr loadPC);

    /** Rename: the value predict() WOULD return for this load PC ignoring the
     *  confidence gate, i.e. the current content of the load's bound slot.
     *  valid=false when the load has no binding yet. Used to snapshot what
     *  the prediction was at rename so confidence can be trained against it
     *  at commit (see commitLoad). Does not touch LRU. */
    MrnPrediction peek(Addr loadPC) const;

    /** Commit: a retiring store deposits its value for an address. */
    void commitStore(Addr effAddr, RegVal value);

    /** Commit: bind a load PC to the producer slot and train confidence.
     *
     *  Confidence must measure "would the load's rename-time prediction have
     *  been correct". With train_on_snapshot=true (the default) it is trained
     *  against snap_value -- the value the prediction actually used, captured
     *  at rename -- rather than valueFile[slot] as of commit, which the
     *  producing store may already have refreshed. That commit-time refresh
     *  is what made a changing recurrence report a spurious match every
     *  iteration. snap_valid=false means the load had no rename-time
     *  prediction (first binding / rebind), so there is nothing to validate
     *  and confidence is left as bound. With train_on_snapshot=false the old
     *  commit-time comparison is restored (for A/B). */
    void commitLoad(Addr loadPC, Addr effAddr, RegVal realValue, bool isSpGp,
                    bool train_on_snapshot = true, bool snap_valid = false,
                    RegVal snap_value = 0);

    /** Writeback: reset a load PC's confidence (loop-safety). */
    void mispredict(Addr loadPC);

    /** Correlator (LSQ-forward): record that a load PC forwarded
     *  from a store PC (learned at the LSQ store->load forward event). */
    void trainForward(Addr loadPC, Addr storePC);

    /** Correlator: the store PC most recently bound to this load PC, or 0 if
     *  none is known. Used at rename to find an in-flight producing store. */
    Addr predictProducerPC(Addr loadPC);

    /** Correlator: the bound store PC for this load PC (0 if none), WITHOUT
     *  touching LRU. For measuring the correlator's prediction against the
     *  true producer at the LSQ forward; must not perturb replacement. */
    Addr peekProducerPC(Addr loadPC) const;

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

    /** Correlator entry (LSQ-forward): loadPC -> producing storePC.
     * Set-associative, sized and indexed exactly like the load cache. */
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
    /** const overload used by peek(), which must not disturb LRU. */
    const LoadEntry *loadFind(Addr loadPC) const;
    /** Allocate (LRU-evict within the set) a load-cache entry for loadPC. */
    LoadEntry *loadAllocate(Addr loadPC);
    /** Allocate (LRU-evict) a value-file slot and return its index. */
    int valueAllocate();
    /** Find the correlator entry for loadPC, or nullptr on a miss. */
    FwdEntry *fwdFind(Addr loadPC);
    /** const overload used by peekProducerPC (must not touch LRU). */
    const FwdEntry *fwdFind(Addr loadPC) const;
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
    /** Correlator table (LSQ-forward), sized/indexed like the load cache. */
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

    /** Rename: the confidence-independent snapshot of the load's bound slot,
     *  captured so commit can train against what the prediction actually
     *  used rather than the refreshed value file. */
    MrnPrediction
    peek(Addr loadPC) const
    {
        return tables.peek(loadPC);
    }

    /** Whether confidence is trained against the rename snapshot (default)
     *  rather than the commit-time value file. */
    bool
    trainOnSnapshot() const
    {
        return _trainOnSnapshot;
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
    commitLoad(Addr loadPC, Addr effAddr, RegVal realValue, bool isSpGp,
               bool snap_valid = false, RegVal snap_value = 0)
    {
        stats.loadsTrained++;
        tables.commitLoad(loadPC, effAddr, realValue, isSpGp, _trainOnSnapshot,
                          snap_valid, snap_value);
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

    /** Writeback: a forwarded load verified correct (value path). */
    void
    noteCorrect()
    {
        stats.predictionsCorrect++;
    }

    /** Rename (aliasing): record loadPC->storePC, learned from the
     * LSQ store->load forward event. */
    void
    trainForward(Addr loadPC, Addr storePC)
    {
        stats.bindingsLearned++;
        tables.trainForward(loadPC, storePC);
    }

    /** Rename (aliasing): the store PC bound to this load PC, or 0 if
     * none. store_set correlation is a stub and always returns 0 (no
     * producer). */
    Addr
    predictProducerPC(Addr loadPC)
    {
        if (_useStoreSet) {
            return 0;
        }
        return tables.predictProducerPC(loadPC);
    }

    /** COVERAGE: at each LSQ store->load forward, score the correlator's
     *  current binding against the store it ACTUALLY forwarded from. The
     *  denominator is true forwards -- "of real forwards, how many does the
     *  correlator already capture". Called just before trainForward updates
     *  the binding. */
    void
    noteProducerCoverage(Addr loadPC, Addr actualStorePC)
    {
        const Addr pred = tables.peekProducerPC(loadPC);
        if (pred == 0) {
            stats.producerCoverageUntrained++;
        } else if (pred == actualStorePC) {
            stats.producerCoverageCorrect++;
        } else {
            stats.producerCoverageWrong++;
        }
    }

    /** ACCURACY (denominator = predictions MADE): the correlator produced a
     *  storePC prediction for a load at rename. Counted at the prediction
     *  site; the outcome is resolved later by noteProducerPredictOutcome. */
    void
    noteProducerPredictMade()
    {
        stats.producerPredictMade++;
    }

    /** ACCURACY: resolve a made prediction at writeback -- confirmed = the
     *  load actually forwarded from the predicted storePC. */
    void
    noteProducerPredictOutcome(bool confirmed)
    {
        if (confirmed) {
            stats.producerPredictCorrect++;
        } else {
            stats.producerPredictWrong++;
        }
    }

    /** ACCURACY: a made prediction whose load was squashed before it could be
     *  validated at writeback. Counted separately (like the coverage buckets)
     *  so accuracy can be computed either way:
     *    incl. squashed = correct / made
     *    excl. squashed = correct / (correct + wrong)
     *  where made == correct + wrong + squashed. */
    void
    noteProducerPredictSquashed()
    {
        stats.producerPredictSquashed++;
    }

    /** Rename: a load was forwarded via the value snapshot. */
    void
    noteForwardValue()
    {
        stats.forwardsValue++;
    }

    /** Rename: a load was aliased to an in-flight producer's physreg. */
    void
    noteForwardAlias()
    {
        stats.forwardsAlias++;
    }

    /** Rename: a producer-aliasing attempt was abandoned after the correlator
     *  made a prediction (producerPredictMade) but before the load was aliased
     *  (forwardsAlias). The four outcomes below plus forwardsAlias partition
     *  every made prediction:
     *    producerPredictMade == forwardsAlias
     *        + aliasNoInflightStore + aliasStoreDataNotInt
     *        + aliasProducerUnusable + aliasStaleRejected
     *  @{ */
    /** No usable in-flight store with the predicted PC was in the store
     *  queue, so there was nothing to alias to. */
    void
    noteAliasNoInflightStore()
    {
        stats.aliasNoInflightStore++;
    }
    /** The located store's data source register was not integer. */
    void
    noteAliasStoreDataNotInt()
    {
        stats.aliasStoreDataNotInt++;
    }
    /** The producer physreg was invalid, fixed-mapping, or already dead. */
    void
    noteAliasProducerUnusable()
    {
        stats.aliasProducerUnusable++;
    }
    /** The producer was stale and aliasRequireCurrentProducer refused it. */
    void
    noteAliasStaleRejected()
    {
        stats.aliasStaleRejected++;
    }
    /** @} */

    /** Writeback: an aliased load verified correct. */
    void
    noteAliasCorrect()
    {
        stats.aliasVerifyCorrect++;
    }

    /** Rename (diagnostic): whether the producer alias resolved to a physreg
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
     *  the load took the alias path, false for the value path;
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

    /** Aliasing: reject an alias whose rename-map lookup no longer
     * matches the physreg the located store captured. See the param
     * description. */
    bool
    aliasRequireCurrentProducer() const
    {
        return _aliasRequireCurrentProducer;
    }

    /** Whether the value-forwarding path is enabled. */
    bool
    valueForwardingEnabled() const
    {
        return _enableValueForwarding;
    }

    /** Whether the producer-aliasing path is enabled. */
    bool
    aliasingEnabled() const
    {
        return _enableProducerAliasing;
    }

    /** Whether the value-file rendezvous correlation is active
     *  (mrnCorrelation == value_file). */
    bool
    valueFileEnabled() const
    {
        return _useValueFile;
    }

    /** Value file: forward the producer physreg's value when it is
     *  already ready at the load's rename. */
    bool
    vfForwardProducerValue() const
    {
        return _vfForwardProducerValue;
    }

    /** Value file: forward the last value from a self-bound cell. */
    bool
    vfForwardLastValue() const
    {
        return _vfForwardLastValue;
    }

    /** Per-mode index for the vfPredict* stat vectors. */
    enum VfMode
    {
        VfModeAlias = 0,
        VfModeProducerValue,
        VfModeLastValue,
        VfModeCount
    };

    /** Value file (rename): a store deposits its producer physreg (and,
     *  when readyValue is non-null, the already-ready value) into its
     *  named cell. Counts the deposit and whether a value rode along. */
    MrnVfRef
    vfStoreRename(Addr pc, PhysRegIdPtr reg, InstSeqNum sn, const RegVal *rv)
    {
        stats.vfDepositsPtr++;
        if (rv) {
            stats.vfDepositsWithValue++;
        }
        return vfTables.storeRename(pc, reg, sn, rv);
    }

    /** Value file (rename): look up a load PC's bound cell. Callers should
     *  invoke vfNoteBelowConf() when the returned cell is bound but not
     *  yet confident, so a prediction was suppressed. */
    MrnVfCellRead
    vfLoadRename(Addr pc)
    {
        return vfTables.loadRename(pc);
    }

    /** Value file (address resolution): a store publishes its cell into
     *  the store cache. Counts the publish, or its suppression (stale
     *  reference / a program-order-younger occupant already holds the
     *  address). */
    void
    vfStoreAddrResolved(const MrnVfRef &ref, Addr ea, InstSeqNum sn)
    {
        if (vfTables.storeAddrResolved(ref, ea, sn)) {
            stats.vfScPublishes++;
        } else {
            stats.vfScPublishSuppressed++;
        }
    }

    /** Value file (address resolution): a load probes the store cache and
     *  rebinds or self-binds per the rendezvous rules. Counts the probe
     *  outcome (hit/miss/dead-channel) and any rebind/self-bind. */
    void
    vfLoadAddrResolved(Addr pc, Addr ea)
    {
        const MrnVfProbeResult result = vfTables.loadAddrResolved(pc, ea);
        if (result.scHitDeadChannel) {
            stats.vfScProbeDeadChannel++;
        }
        switch (result.outcome) {
            case MrnVfProbeResult::SameBinding:
                stats.vfScProbeHits++;
                break;
            case MrnVfProbeResult::Rebound:
                stats.vfScProbeHits++;
                stats.vfRebinds++;
                break;
            case MrnVfProbeResult::SelfBound:
                stats.vfScProbeMisses++;
                stats.vfSelfBinds++;
                break;
            case MrnVfProbeResult::AlreadySelfBound:
                stats.vfScProbeMisses++;
                break;
        }
    }

    /** Value file (writeback): update a self-bound cell with the load's
     *  resolved value (last-value forwarding). */
    void
    vfLoadDataResolved(Addr pc, RegVal v)
    {
        vfTables.loadDataResolved(pc, v);
    }

    /** Value file (writeback): train the bound cell's confidence counter
     *  against the verified outcome. */
    void
    vfTrainVerify(Addr pc, const MrnVfRef &ref, bool correct)
    {
        vfTables.trainVerify(pc, ref, correct);
    }

    /** ACCURACY: a value-file prediction was made for a given mode
     *  (VfMode). */
    void
    vfNotePredictMade(int mode)
    {
        stats.vfPredictMade[mode]++;
    }

    /** ACCURACY: resolve a made value-file prediction at verification. */
    void
    vfNotePredictOutcome(int mode, bool correct)
    {
        if (correct) {
            stats.vfPredictCorrect[mode]++;
        } else {
            stats.vfPredictWrong[mode]++;
        }
    }

    /** A made value-file prediction whose load was squashed before it
     *  could be verified. */
    void
    vfNotePredictSquashed(int mode)
    {
        stats.vfPredictSquashed[mode]++;
    }

    /** Value file: a rename-time lookup was bound but below the
     *  confidence threshold, so no prediction was made. */
    void
    vfNoteBelowConf()
    {
        stats.vfBelowConfSuppressed++;
    }

    /** Value file: a shadow (non-forwarding) comparison against the true
     *  load value, used to measure would-be accuracy without forwarding. */
    void
    vfNoteShadow(bool correct)
    {
        if (correct) {
            stats.vfShadowCorrect++;
        } else {
            stats.vfShadowWrong++;
        }
    }

    /** Value file: a shadow comparison was skipped because the producer
     *  value was not available. */
    void
    vfNoteShadowSkipped()
    {
        stats.vfShadowSkipped++;
    }

  private:
    MrnTables tables;
    MrnValueFileTables vfTables;
    const bool _aliasRequireCurrentProducer;

    const bool _trainOnSnapshot;

    const bool _predictIntLoadsOnly;
    /** Value-forwarding path enabled. */
    const bool _enableValueForwarding;
    /** Producer-aliasing path enabled. */
    const bool _enableProducerAliasing;
    /** mrnCorrelation == store_set (stub: predictProducerPC returns 0). */
    const bool _useStoreSet;
    /** mrnCorrelation == value_file (rendezvous model). */
    const bool _useValueFile;
    /** Value file: forward the producer physreg's value when it is
     *  already ready at the load's rename. */
    const bool _vfForwardProducerValue;
    /** Value file: forward the last value from a self-bound cell. */
    const bool _vfForwardLastValue;

    /** Training statistics (Garfield MRN). */
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
        /** High-confidence loads forwarded via the value snapshot. */
        statistics::Scalar forwardsValue;
        /** High-confidence loads aliased to an in-flight producer's physreg
         *  (producer aliasing). */
        statistics::Scalar forwardsAlias;
        /** Aliased loads that verified correct at writeback. */
        statistics::Scalar aliasVerifyCorrect;
        /** Aliased loads that verified wrong and forced a squash. */
        statistics::Scalar aliasMispredicts;
        /** Aliased loads whose producer was not yet ready at the load's
         *  writeback, so verification waited for the producer (the
         * producer-aliasing ingenuity: the alias bought work the value path
         * could not). */
        statistics::Scalar aliasVerifyWaitedForProducer;
        /** loadPC->storePC correlator bindings learned from LSQ forwarding. */
        statistics::Scalar bindingsLearned;

        /** COVERAGE (denominator = true LSQ forwards): at each forward the
         *  correlator's binding matched the store the load forwarded from. */
        statistics::Scalar producerCoverageCorrect;
        /** ...the binding existed but named a different store PC. */
        statistics::Scalar producerCoverageWrong;
        /** ...no binding existed yet (first forward for this load PC). */
        statistics::Scalar producerCoverageUntrained;

        /** ACCURACY (denominator = predictions MADE): the correlator produced
         *  a storePC prediction for a load at rename. */
        statistics::Scalar producerPredictMade;
        /** ...and the load actually forwarded from that predicted store PC. */
        statistics::Scalar producerPredictCorrect;
        /** ...and it did NOT (forwarded from another store, or not at all). */
        statistics::Scalar producerPredictWrong;
        /** ...and the load was squashed before writeback could validate it
         *  (made == correct + wrong + squashed). */
        statistics::Scalar producerPredictSquashed;

        /** Value-path forwards discarded before they could verify,
         *  indexed by MrnSquashReason. */
        statistics::Vector predictionsSquashedValue;

        /** Alias-path forwards discarded before they could verify,
         *  indexed by MrnSquashReason. */
        statistics::Vector predictionsSquashedAlias;

        /** Diagnostic: alias attempts whose rename-map lookup matched the
         *  located store's own captured data physreg. */
        statistics::Scalar aliasProducerCurrent;

        /** Diagnostic: alias attempts where it did NOT match (the data arch
         *  reg was redefined between the store's rename and the load's). */
        statistics::Scalar aliasProducerStale;

        /** Breakdown of made predictions that did NOT become an alias, one
         *  counter per abandon path in tryMemRenameAlias. With forwardsAlias
         *  these partition producerPredictMade. */
        /** ...no usable in-flight store with the predicted PC. */
        statistics::Scalar aliasNoInflightStore;
        /** ...the located store's data source register was not integer. */
        statistics::Scalar aliasStoreDataNotInt;
        /** ...the producer physreg was invalid/fixed/dead. */
        statistics::Scalar aliasProducerUnusable;
        /** ...the producer was stale and the current-producer gate refused. */
        statistics::Scalar aliasStaleRejected;

        /** Diagnostic: alias verify outcome bucketed by staleness --
         *  {currentWrong, currentCorrect, staleWrong, staleCorrect}. */
        statistics::Vector aliasOutcomeByStaleness;

        /** Value-file predictions made, by forwarding mode (VfMode). */
        statistics::Vector vfPredictMade;
        /** ...that verified correct, by forwarding mode. */
        statistics::Vector vfPredictCorrect;
        /** ...that verified wrong and forced a squash, by forwarding
         *  mode. */
        statistics::Vector vfPredictWrong;
        /** ...whose load was squashed before it could verify, by
         *  forwarding mode. */
        statistics::Vector vfPredictSquashed;

        /** Value file: store renames that deposited a producer register
         *  pointer into a cell. */
        statistics::Scalar vfDepositsPtr;
        /** Value file: store renames whose deposit also included an
         *  already-ready value. */
        statistics::Scalar vfDepositsWithValue;
        /** Value file: store address resolutions that published a cell
         *  into the store cache. */
        statistics::Scalar vfScPublishes;
        /** Value file: store cache publishes suppressed by a stale
         *  reference or a program-order-younger occupant. */
        statistics::Scalar vfScPublishSuppressed;
        /** Value file: load address resolutions that hit the store
         *  cache. */
        statistics::Scalar vfScProbeHits;
        /** Value file: load address resolutions that missed the store
         *  cache. */
        statistics::Scalar vfScProbeMisses;
        /** Value file: store-cache hits whose cell had been reallocated
         *  since (dead channel, treated as a miss). */
        statistics::Scalar vfScProbeDeadChannel;
        /** Value file: loads rebound to a different cell at address
         *  resolution. */
        statistics::Scalar vfRebinds;
        /** Value file: loads newly self-bound to their own cell at
         *  address resolution. */
        statistics::Scalar vfSelfBinds;
        /** Value file: rename-time lookups that were bound but below the
         *  confidence threshold, so no prediction was made. */
        statistics::Scalar vfBelowConfSuppressed;
        /** Value file: shadow comparisons that matched the true load
         *  value. */
        statistics::Scalar vfShadowCorrect;
        /** Value file: shadow comparisons that did not match the true
         *  load value. */
        statistics::Scalar vfShadowWrong;
        /** Value file: shadow comparisons skipped because the producer
         *  value was not available. */
        statistics::Scalar vfShadowSkipped;

    } stats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MEM_RENAME_PREDICTOR_HH__
