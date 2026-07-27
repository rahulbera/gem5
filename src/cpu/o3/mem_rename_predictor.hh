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

// Forward declaration of the generated params struct. The SimObject
// constructor stays out-of-line (in mem_rename_predictor_sim.cc) so this
// header does not pull in the generated params/MemRenamePredictor.hh.
struct MemRenamePredictorParams;

namespace o3
{

/**
 * Memory-renaming predictor SimObject: the value-file rendezvous model
 * (see mem_rename_valuefile.hh). A thin wrapper that owns the params-free
 * MrnValueFileTables core -- where all behaviour lives, unit-testable
 * without the params/SimObject machinery -- plus the statistics and the
 * configuration flags the pipeline stages consult.
 */
class MemRenamePredictor : public SimObject
{
  public:
    MemRenamePredictor(const MemRenamePredictorParams &p);

    /** Writeback: a forwarded load mispredicted. Reset confidence
     *  (loop-safety) and record the flush cost (squashedInsts = the load plus
     *  every younger in-flight instruction, all discarded by the squash). */
    void
    mispredict(Addr loadPC, uint64_t squashedInsts)
    {
        stats.mispredicts++;
        stats.squashedInsts += squashedInsts;
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

    /** Rename: a load was forwarded a value at rename. */
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

    /** Writeback: an aliased load verified correct. */
    void
    noteAliasCorrect()
    {
        stats.aliasVerifyCorrect++;
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
    }

    /** Whether the producer-aliasing path is enabled. */
    bool
    aliasingEnabled() const
    {
        return _enableProducerAliasing;
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
    /** Verify site: did the load's line change since its previous
     *  instance? The address-instability input to the stability gate's
     *  strike test (vfTrainVerify). */
    bool
    vfCellAddrChanged(const MrnVfRef &ref) const
    {
        return vfTables.cellAddrChanged(ref);
    }

    /** Split a last-value verify outcome by address stability -- the
     *  signal the stability gate strikes on. */
    void
    vfNoteLastValueAddrClass(bool correct, bool changed)
    {
        if (correct) {
            (changed ? stats.lvCorrectAddrChanged : stats.lvCorrectAddrSame)++;
        } else {
            (changed ? stats.lvWrongAddrChanged : stats.lvWrongAddrSame)++;
        }
    }

    /** A confident last-value consumption skipped due to probation. */
    void
    vfNoteLvProbationSuppressed()
    {
        stats.lvProbationSuppressed++;
    }

    /** Value file (writeback): train the bound cell's confidence counter
     *  against the verified outcome. */
    void
    vfTrainVerify(Addr pc, const MrnVfRef &ref, bool correct,
                  bool addrChanged = false)
    {
        vfTables.trainVerify(pc, ref, correct, addrChanged);
        if (vfTables.lastStrikeDisabled()) {
            stats.lvStrikes++;
        }
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

    /** Value file: a confident, ptr-bound cell whose producer physreg
     *  failed the rename-time liveness guards (dead, fixed-mapping, or
     *  non-integer) -- nothing was usable to alias or value-forward
     *  from, so the binding was silently unused. */
    void
    vfNotePtrUnusable()
    {
        stats.vfPtrUnusable++;
    }

    /** Value file: a confident binding whose applicable consumption mode
     *  is disabled by configuration (e.g. a ready producer with
     *  producer-value forwarding off, or a value-only cell with
     *  last-value forwarding off) -- nothing was consumed. Lets
     *  single-mode configurations see what they leave on the table. */
    void
    vfNoteConfidentUnconsumed()
    {
        stats.vfConfidentUnconsumed++;
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
    MrnValueFileTables vfTables;
    /** Producer-aliasing path enabled. */
    const bool _enableProducerAliasing;
    /** Value file: forward the producer physreg's value when it is
     *  already ready at the load's rename. */
    const bool _vfForwardProducerValue;
    /** Value file: forward the last value from a self-bound cell. */
    const bool _vfForwardLastValue;

    /** Training statistics (Garfield MRN). */
    struct MemRenameStats : public statistics::Group
    {
        explicit MemRenameStats(statistics::Group *parent);
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
        /** High-confidence loads forwarded a value at rename. */
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
        /** Value-path forwards discarded before they could verify,
         *  indexed by MrnSquashReason. */
        statistics::Vector predictionsSquashedValue;

        /** Alias-path forwards discarded before they could verify,
         *  indexed by MrnSquashReason. */
        statistics::Vector predictionsSquashedAlias;

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
        /** Confident pointer bindings unusable at rename: producer
         *  physreg dead, fixed-mapping, or non-integer. */
        statistics::Scalar vfPtrUnusable;
        /** Confident bindings whose applicable consumption mode was
         *  disabled by configuration, so nothing was consumed. */
        statistics::Scalar vfConfidentUnconsumed;
        /** Value file: shadow comparisons that matched the true load
         *  value. */
        statistics::Scalar vfShadowCorrect;
        /** Value file: shadow comparisons that did not match the true
         *  load value. */
        statistics::Scalar vfShadowWrong;
        /** Value file: shadow comparisons skipped because the producer
         *  value was not available. */
        statistics::Scalar vfShadowSkipped;
        /** Last-value verify outcomes split by address stability -- the
         *  signal the stability gate strikes on: did the load resolve to
         *  a different line than its previous instance? */
        statistics::Scalar lvWrongAddrChanged;
        statistics::Scalar lvWrongAddrSame;
        statistics::Scalar lvCorrectAddrChanged;
        statistics::Scalar lvCorrectAddrSame;
        /** Address-instability strike probation (last-value gate). */
        statistics::Scalar lvStrikes;
        statistics::Scalar lvProbationSuppressed;

    } stats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MEM_RENAME_PREDICTOR_HH__
