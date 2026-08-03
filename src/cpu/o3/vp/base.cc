#include "cpu/o3/vp/base.hh"

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/op_class.hh"
#include "cpu/reg_class.hh"
#include "params/BaseValuePredictor.hh"

namespace gem5
{
namespace o3
{

namespace
{

/** Class dispatch for VpClassifierInfo::instClass (design doc docs/
 *  superpowers/specs/2026-08-04-evtage-design.md, "Class dispatch");
 *  see vp_types.hh's VpInstClass doc comment for the Undef fallback's
 *  scope (folds in direct calls/conditional branches, which CVP
 *  itself dispatches to a deterministic-false arm this port's
 *  EVtageClassifier has no field to express). */
VpInstClass
classifyInst(const DynInstPtr &inst)
{
    if (inst->isLoad()) {
        return VpInstClass::Load;
    }
    if (inst->isIndirectCtrl() && inst->isCall()) {
        return VpInstClass::IndirectCall;
    }
    if (inst->isStore()) {
        return VpInstClass::Store;
    }
    switch (inst->opClass()) {
        case IntAluOp:
            return VpInstClass::Alu;
        case IntMultOp:
        case IntDivOp:
        case FloatAddOp:
        case FloatCmpOp:
        case FloatCvtOp:
        case FloatMultOp:
        case FloatMultAccOp:
        case FloatDivOp:
        case FloatMiscOp:
        case FloatSqrtOp:
            return VpInstClass::SlowAlu;
        default:
            return VpInstClass::Undef;
    }
}

/** CVP's NbOperand (design doc, "Operand-count mapping"): the count of
 *  integer, non-flag source registers. Testing is(IntRegClass)
 *  positively excludes CCRegClass (ARM condition-code sources, a
 *  distinct reg class -- see src/arch/arm/regs/cc.hh) as well as
 *  Float/Vec sources, matching the design doc's rationale for not
 *  using the raw numSrcRegs() count. */
unsigned
countIntOperands(const DynInstPtr &inst)
{
    unsigned count = 0;
    const int n = static_cast<int>(inst->numSrcRegs());
    for (int i = 0; i < n; i++) {
        if (inst->srcRegIdx(i).is(IntRegClass)) {
            count++;
        }
    }
    return count;
}

} // anonymous namespace

BaseValuePredictor::BaseValuePredictor(const BaseValuePredictorParams &p)
    : SimObject(p),
      _onlyLoads(p.onlyLoads),
      _scalarOnly(p.scalarOnly),
      _historyPathBits(p.historyPathBits),
      stats(this)
{
    fatal_if(p.historyPathBits == 0 || p.historyPathBits > 16,
             "historyPathBits (%u) must be in [1, 16] (path register "
             "is 16 bits wide)",
             p.historyPathBits);
}

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
    VpLookupContext ctx{inst->pcState().instAddr(), inst->pcState().microPC(),
                        inst->vpHistSnap(), inst->threadNumber};
    VpPredictResult result = predictImpl(ctx);
    inst->setVpToken(result.token);
    if (result.value) {
        stats.predictionsMade++;
        inst->setVpPredicted();
        inst->setVpPredVal(*result.value);
    }
    return result.value;
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
    VpLookupContext ctx{inst->pcState().instAddr(), inst->pcState().microPC(),
                        inst->vpHistSnap(), inst->threadNumber};
    VpClassifierInfo classifier;
    classifier.instClass = classifyInst(inst);
    classifier.memSrcLevel = inst->memSrcLevel();
    classifier.nbOperand = countIntOperands(inst);
    // By the time train() runs (writeback for writeback-trained
    // predictors, commit for trainsAtCommit ones), a delivered
    // prediction has already been verified -- vpResolved() is always
    // true when vpPredicted() is true (a delivered-and-wrong
    // prediction squashes inclusively and never reaches train() for
    // this same instance; see correctiveReset()'s doc comment).
    classifier.deliveredCorrect = inst->vpPredicted() && inst->vpResolved() &&
                                  inst->vpPredVal() == actualValue;
    trainImpl(ctx, actualValue, inst->vpToken(), classifier);
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

void
BaseValuePredictor::notifyControlFlow(ThreadID tid, bool isCond,
                                      bool predTaken, Addr target)
{
    if (isCond) {
        vpHist[tid].branchShift(predTaken);
    }
    if (predTaken) {
        vpHist[tid].takenTarget(target, _historyPathBits);
    }
}

void
BaseValuePredictor::snapshotFor(const DynInstPtr &inst) const
{
    inst->setVpHistSnap(vpHist[inst->threadNumber].state);
}

void
BaseValuePredictor::restoreHistory(ThreadID tid, const VpHistSnapshot &snap)
{
    vpHist[tid].restore(snap);
}

void
BaseValuePredictor::countHistoryRestore(VpHistInitiator initiator)
{
    stats.historyRestores[static_cast<unsigned>(initiator)]++;
}

bool
BaseValuePredictor::correctiveReset(uint64_t token)
{
    bool live = correctiveResetImpl(token);
    if (!live) {
        stats.correctiveResetStale++;
    }
    return live;
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
      ADD_STAT(historyRestores, statistics::units::Count::get(),
               "History-subsystem restores (usesHistory() predictors "
               "only), by initiating pipeline redirect; 'missed' counts "
               "an initiator that could not resolve a restore snapshot"),
      ADD_STAT(correctiveResetStale, statistics::units::Count::get(),
               "correctiveReset() calls whose token no longer matched a "
               "live provider (trainAtCommit predictors only)"),
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

    static const char *hist_restore_names[] = {
        "branch", "decode", "inclusive", "trap", "squashAfter", "missed"};
    historyRestores.init(6).flags(statistics::total);
    for (int i = 0; i < 6; i++) {
        historyRestores.subname(i, hist_restore_names[i]);
    }

    coverage.flags(statistics::total);
    coverage = predictionsCorrect / (eligibleLoads + eligibleNonLoads);
    accuracy.flags(statistics::total);
    accuracy = predictionsCorrect /
               (predictionsCorrect + predictionsWrong);
}

} // namespace o3
} // namespace gem5
