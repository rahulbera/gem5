#include "cpu/o3/vp/base.hh"

#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/vp/vp_key.hh"
#include "params/BaseValuePredictor.hh"

namespace gem5
{
namespace o3
{

BaseValuePredictor::BaseValuePredictor(const BaseValuePredictorParams &p)
    : SimObject(p),
      _onlyLoads(p.onlyLoads),
      _scalarOnly(p.scalarOnly),
      stats(this)
{}

bool
BaseValuePredictor::eligible(const DynInstPtr &inst) const
{
    if (inst->numDestRegs() != 1) {
        return false;
    }
    if (inst->isSerializing() || inst->isSerializeBefore() ||
        inst->isSerializeAfter() || inst->isNonSpeculative() ||
        inst->isReadBarrier() || inst->isWriteBarrier() ||
        inst->isAtomic() || inst->isStoreConditional()) {
        return false;
    }
    PhysRegIdPtr dest = inst->renamedDestIdx(0);
    if (!dest || !dest->is(IntRegClass) || dest->isFixedMapping()) {
        return false;
    }
    return true;
}

bool
BaseValuePredictor::inScope(const DynInstPtr &inst) const
{
    if (_onlyLoads && !inst->isLoad()) {
        return false;
    }
    return eligible(inst);
}

std::optional<RegVal>
BaseValuePredictor::predict(const DynInstPtr &inst)
{
    if (!inScope(inst)) {
        return std::nullopt;
    }
    const Addr key = vpKey(inst->pcState().instAddr(),
                           inst->pcState().microPC());
    std::optional<RegVal> v = predictImpl(key);
    if (v) {
        stats.predictionsMade++;
        inst->setVpPredicted();
        inst->setVpPredVal(*v);
    }
    return v;
}

void
BaseValuePredictor::train(const DynInstPtr &inst, RegVal actualValue)
{
    if (!inScope(inst)) {
        return;
    }
    if (inst->isLoad()) {
        stats.eligibleLoads++;
    } else {
        stats.eligibleNonLoads++;
    }
    trainImpl(vpKey(inst->pcState().instAddr(), inst->pcState().microPC()),
              actualValue);
}

void
BaseValuePredictor::verifyResult(const DynInstPtr &inst, bool correct,
                                 uint64_t flushed)
{
    const unsigned lvl =
        inst->memSrcLevel() > 4 ? 4 : inst->memSrcLevel();
    if (correct) {
        stats.predictionsCorrect++;
        if (inst->isLoad()) {
            stats.predictedLevelCorrect[lvl]++;
        }
    } else {
        stats.predictionsWrong++;
        stats.squashedInsts += flushed;
        if (inst->isLoad()) {
            stats.predictedLevelWrong[lvl]++;
            stats.wrongFlushedByLevel[lvl] += flushed;
        }
    }
}

void
BaseValuePredictor::notifySquashed(const DynInstPtr &)
{
    // The instruction handle is part of the API for derived predictors
    // (and future per-reason buckets); the base count needs only the
    // event. Unnamed to satisfy -Werror=unused-parameter.
    stats.predictionsSquashed++;
}

BaseValuePredictor::VpStats::VpStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(eligibleLoads, statistics::units::Count::get(),
               "In-scope loads reaching the train site (includes "
               "MRN-claimed loads; excludes already-squashed ones)"),
      ADD_STAT(eligibleNonLoads, statistics::units::Count::get(),
               "In-scope non-loads reaching the train site (structurally "
               "zero in loads-only mode)"),
      ADD_STAT(predictionsMade, statistics::units::Count::get(),
               "Value predictions consumed into a renamed dest at rename"),
      ADD_STAT(predictionsCorrect, statistics::units::Count::get(),
               "Consumed predictions that verified correct at writeback"),
      ADD_STAT(predictionsWrong, statistics::units::Count::get(),
               "Consumed predictions that verified wrong and forced an "
               "inclusive squash"),
      ADD_STAT(predictionsSquashed, statistics::units::Count::get(),
               "Consumed predictions discarded before they could verify"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Instructions discarded by VP misprediction squashes "
               "(inclusive of the mispredicted instruction; counted at "
               "verify time, so when an older same-cycle squash wins "
               "precedence the flush is over-attributed -- same "
               "convention as MRN's squashedInsts)"),
      ADD_STAT(coverage, statistics::units::Ratio::get(),
               "Verified-correct predictions over the in-scope "
               "population at the train site"),
      ADD_STAT(accuracy, statistics::units::Ratio::get(),
               "Verified-correct predictions over verified predictions"),
      ADD_STAT(predictedLevelCorrect, statistics::units::Count::get(),
               "Memory-system level that served each correct predicted "
               "load (stlf = store-to-load forward; l2/mem from the "
               "request's own miss depth)"),
      ADD_STAT(predictedLevelWrong, statistics::units::Count::get(),
               "Memory-system level that served each wrong predicted "
               "load (same attribution as predictedLevelCorrect)"),
      ADD_STAT(wrongFlushedByLevel, statistics::units::Count::get(),
               "Instructions flushed by wrong predicted loads, "
               "accumulated by serving level (divide by "
               "predictedLevelWrong for avg flush per wrong; sums to "
               "squashedInsts in loads-only scope -- in "
               "all-instructions scope wrong non-loads contribute to "
               "squashedInsts but not here)")
{
    static const char *level_names[] = {"stlf", "l1d", "l2", "mem",
                                        "unknown"};
    predictedLevelCorrect.init(5).flags(statistics::total);
    predictedLevelWrong.init(5).flags(statistics::total);
    wrongFlushedByLevel.init(5).flags(statistics::total);
    for (int i = 0; i < 5; i++) {
        predictedLevelCorrect.subname(i, level_names[i]);
        predictedLevelWrong.subname(i, level_names[i]);
        wrongFlushedByLevel.subname(i, level_names[i]);
    }

    coverage.flags(statistics::total);
    coverage = predictionsCorrect / (eligibleLoads + eligibleNonLoads);
    accuracy.flags(statistics::total);
    accuracy = predictionsCorrect /
               (predictionsCorrect + predictionsWrong);
}

} // namespace o3
} // namespace gem5
