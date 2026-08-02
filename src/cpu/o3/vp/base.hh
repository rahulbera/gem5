#ifndef __CPU_O3_VP_BASE_HH__
#define __CPU_O3_VP_BASE_HH__

#include <cstdint>
#include <optional>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "sim/sim_object.hh"

namespace gem5
{

// Forward declaration of the generated params struct. The SimObject
// constructor stays out-of-line (in base.cc) so this header does not
// pull in the generated params/BaseValuePredictor.hh and stays cheap to
// include from pipeline code (rename/iew/lsq).
struct BaseValuePredictorParams;

namespace o3
{

/**
 * Garfield VP: algorithm-agnostic value-predictor base (design doc
 * docs/superpowers/specs/2026-08-02-vp-framework-design.md).
 *
 * The pipeline contract has three operations plus one reserved hook:
 *  - predict() at rename (after renameDestRegs, and only for
 *    instructions MRN did not claim): the base class alone decides
 *    eligibility and scope; a returned value has already been counted
 *    and stamped on the instruction, and the caller forwards it into
 *    the renamed destination (physreg write + scoreboard ready).
 *  - train() at writeback for every in-scope, not-already-squashed
 *    instruction -- whether or not a prediction was made, and including
 *    MRN-claimed loads: the tables stay warm regardless of predictor
 *    engagement.
 *  - verifyResult()/notifySquashed(): outcome accounting from the
 *    verify sites and the squash walks.
 * Derived predictors implement predictImpl()/trainImpl() over the
 * folded (PC, micro-PC) key (vp_key.hh) and keep their table logic in a
 * params-free, unit-tested core class.
 */
class BaseValuePredictor : public SimObject
{
  public:
    BaseValuePredictor(const BaseValuePredictorParams &p);

    /** Rename-time lookup (call only after renameDestRegs). Returns a
     *  value to consume, or nullopt for: ineligible, out of scope,
     *  table miss, or below the confidence threshold. On a hit the
     *  base class counts predictionsMade and stamps
     *  setVpPredicted()/setVpPredVal() on the instruction. */
    std::optional<RegVal> predict(const DynInstPtr &inst);

    /** Writeback-time training with the architected value. Counts the
     *  eligible population (the coverage denominator) and delegates to
     *  trainImpl(). No-op for out-of-scope instructions, so call sites
     *  stay thin. */
    void train(const DynInstPtr &inst, RegVal actualValue);

    /** Verify-site outcome accounting for a consumed prediction.
     *  @param flushed the measured inclusive squash cost when wrong
     *  (0 when correct). Level attribution runs for loads only. */
    void verifyResult(const DynInstPtr &inst, bool correct,
                      uint64_t flushed);

    /** A consumed prediction was discarded before it could verify
     *  (counted, never trained). Called from the ROB/CPU squash walks
     *  under the VpPredicted && !VpResolved exactly-once guard. */
    void notifySquashed(const DynInstPtr &inst);

    /** Reserved hook: predictors carrying speculative state (VTAGE
     *  class) restore it here on any pipeline squash. LVP ignores it. */
    virtual void notifyPipelineSquash() {}

    /** Scope knob: loads only (default) vs all eligible instructions. */
    bool
    onlyLoads() const
    {
        return _onlyLoads;
    }

    /** Eligibility (hard rules) AND scope (onlyLoads). Public so the
     *  IEW non-load site can pre-filter before reading the dest reg. */
    bool inScope(const DynInstPtr &inst) const;

  protected:
    /** Algorithm lookup over the folded key. */
    virtual std::optional<RegVal> predictImpl(Addr key) = 0;
    /** Algorithm training over the folded key. */
    virtual void trainImpl(Addr key, RegVal actualValue) = 0;

    /** Hard eligibility rules: single scalar integer, non-fixed-mapping
     *  destination; not serializing / barrier / non-speculative /
     *  atomic / store-conditional. */
    bool eligible(const DynInstPtr &inst) const;

    const bool _onlyLoads;
    /** Subsumed by the integer-only rule today; retained so the
     *  interface is stable when FP/vector support lands. */
    const bool _scalarOnly;

    struct VpStats : public statistics::Group
    {
        explicit VpStats(statistics::Group *parent);

        /** In-scope instructions reaching the train site (the coverage
         *  denominator), split loads / non-loads. Includes MRN-claimed
         *  loads; excludes already-squashed instructions. */
        statistics::Scalar eligibleLoads;
        statistics::Scalar eligibleNonLoads;
        /** Predictions consumed at rename (speculative path). Identity:
         *  made == correct + wrong + squashed (+- in flight at the
         *  dump boundary). */
        statistics::Scalar predictionsMade;
        statistics::Scalar predictionsCorrect;
        statistics::Scalar predictionsWrong;
        /** Consumed predictions discarded before they could verify. */
        statistics::Scalar predictionsSquashed;
        /** Instructions discarded by VP misprediction squashes
         *  (inclusive: the mispredicted instruction itself counts). */
        statistics::Scalar squashedInsts;
        /** correct / (eligibleLoads + eligibleNonLoads). */
        statistics::Formula coverage;
        /** correct / (correct + wrong). */
        statistics::Formula accuracy;
        /** Memory-system level (stlf/l1d/l2/mem/unknown) that served
         *  each verified predicted load, by outcome; flush cost of
         *  wrong predicted loads by level. */
        statistics::Vector predictedLevelCorrect;
        statistics::Vector predictedLevelWrong;
        statistics::Vector wrongFlushedByLevel;
    } stats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_VP_BASE_HH__
