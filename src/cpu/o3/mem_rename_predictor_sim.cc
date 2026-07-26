#include "cpu/o3/mem_rename_predictor.hh"
#include "params/MemRenamePredictor.hh"

namespace gem5
{
namespace o3
{

MemRenamePredictor::MemRenamePredictor(const MemRenamePredictorParams &p)
    : SimObject(p),
      vfTables(MrnVfConfig{p.vfEntries, p.slcEntries, p.slcAssoc, p.scEntries,
                           p.scAssoc, p.scGranularityBytes, p.confBits,
                           p.confThreshold, p.confInc, p.confDec,
                           p.resetConfOnMispredict, p.lmEntries, p.lmAssoc,
                           p.lvStabilityTarget}),
      _enableProducerAliasing(p.enableProducerAliasing),
      _vfForwardProducerValue(p.vfForwardProducerValue),
      _vfForwardLastValue(p.vfForwardLastValue),
      stats(this)
{}

MemRenamePredictor::MemRenameStats::MemRenameStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(predictionsMade, statistics::units::Count::get(),
               "High-confidence MRN predictions forwarded into a renamed reg"),
      ADD_STAT(predictionsCorrect, statistics::units::Count::get(),
               "Forwarded MRN loads that verified correct at writeback"),
      ADD_STAT(mispredicts, statistics::units::Count::get(),
               "Forwarded MRN loads that verified wrong and forced a squash"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Instructions discarded by MRN misprediction squashes"),
      ADD_STAT(forwardsValue, statistics::units::Count::get(),
               "High-confidence loads forwarded a value at rename "
               "(producer-value or last-value mode)"),
      ADD_STAT(forwardsAlias, statistics::units::Count::get(),
               "High-confidence loads aliased to an in-flight producer's "
               "physreg (producer aliasing)"),
      ADD_STAT(aliasVerifyCorrect, statistics::units::Count::get(),
               "Aliased loads that verified correct at writeback"),
      ADD_STAT(aliasMispredicts, statistics::units::Count::get(),
               "Aliased loads that verified wrong and forced a squash"),
      ADD_STAT(aliasVerifyWaitedForProducer, statistics::units::Count::get(),
               "Aliased loads whose producer was not ready at writeback, so "
               "verification waited for the producer"),
      ADD_STAT(predictionsSquashedValue, statistics::units::Count::get(),
               "Value-mode forwards squashed before they could verify, "
               "by squash reason"),
      ADD_STAT(predictionsSquashedAlias, statistics::units::Count::get(),
               "Alias-mode forwards squashed before they could verify, "
               "by squash reason"),
      ADD_STAT(vfPredictMade, statistics::units::Count::get(),
               "Value-file predictions made, by forwarding mode"),
      ADD_STAT(vfPredictCorrect, statistics::units::Count::get(),
               "Value-file predictions that verified correct, by "
               "forwarding mode"),
      ADD_STAT(vfPredictWrong, statistics::units::Count::get(),
               "Value-file predictions that verified wrong and forced a "
               "squash, by forwarding mode"),
      ADD_STAT(vfPredictSquashed, statistics::units::Count::get(),
               "Value-file predictions squashed before they could verify, "
               "by forwarding mode"),
      ADD_STAT(vfDepositsPtr, statistics::units::Count::get(),
               "Value-file store renames that deposited a producer "
               "register pointer into a cell"),
      ADD_STAT(vfDepositsWithValue, statistics::units::Count::get(),
               "Value-file store renames whose deposit also included an "
               "already-ready value"),
      ADD_STAT(vfScPublishes, statistics::units::Count::get(),
               "Value-file store address resolutions that published a "
               "cell into the store cache"),
      ADD_STAT(vfScPublishSuppressed, statistics::units::Count::get(),
               "Value-file store cache publishes suppressed by a stale "
               "reference or a program-order-younger occupant"),
      ADD_STAT(vfScProbeHits, statistics::units::Count::get(),
               "Value-file load address resolutions that hit the store "
               "cache"),
      ADD_STAT(vfScProbeMisses, statistics::units::Count::get(),
               "Value-file load address resolutions that missed the "
               "store cache"),
      ADD_STAT(vfScProbeDeadChannel, statistics::units::Count::get(),
               "Value-file store-cache hits whose cell had been "
               "reallocated since (dead channel, treated as a miss)"),
      ADD_STAT(vfRebinds, statistics::units::Count::get(),
               "Value-file loads rebound to a different cell at address "
               "resolution"),
      ADD_STAT(vfSelfBinds, statistics::units::Count::get(),
               "Value-file loads newly self-bound to their own cell at "
               "address resolution"),
      ADD_STAT(vfBelowConfSuppressed, statistics::units::Count::get(),
               "Value-file rename-time lookups that were bound but below "
               "the confidence threshold, so no prediction was made"),
      ADD_STAT(vfPtrUnusable, statistics::units::Count::get(),
               "Confident pointer bindings unusable at rename: producer "
               "physreg dead, fixed-mapping, or non-integer"),
      ADD_STAT(vfConfidentUnconsumed, statistics::units::Count::get(),
               "Confident bindings whose applicable consumption mode was "
               "disabled by configuration, so nothing was consumed"),
      ADD_STAT(vfShadowCorrect, statistics::units::Count::get(),
               "Value-file shadow comparisons that matched the true load "
               "value"),
      ADD_STAT(vfShadowWrong, statistics::units::Count::get(),
               "Value-file shadow comparisons that did not match the "
               "true load value"),
      ADD_STAT(vfShadowSkipped, statistics::units::Count::get(),
               "Value-file shadow comparisons skipped because the "
               "producer value was not available"),
      ADD_STAT(lmStoreProbes, statistics::units::Count::get(),
               "Store address resolutions that probed the load-address "
               "monitor (probe-only observation)"),
      ADD_STAT(lmStoreHits, statistics::units::Count::get(),
               "Load-address monitor probes that hit a live self-bound "
               "cell reference"),
      ADD_STAT(lvWrongStoreWritten, statistics::units::Count::get(),
               "Wrong last-value forwards with an observed older "
               "store-write since the cell's last refill (catchable by "
               "store-write invalidation)"),
      ADD_STAT(lvWrongNoStore, statistics::units::Count::get(),
               "Wrong last-value forwards with no observed store-write "
               "(unreachable by store-write invalidation)"),
      ADD_STAT(lvCorrectStoreWritten, statistics::units::Count::get(),
               "Correct last-value forwards with an observed older "
               "store-write (would-be collateral of invalidating)"),
      ADD_STAT(lvCorrectNoStore, statistics::units::Count::get(),
               "Correct last-value forwards with no observed store-write"),
      ADD_STAT(lvWrongAddrChanged, statistics::units::Count::get(),
               "Wrong last-value forwards whose load resolved to a "
               "different line than its previous instance (address "
               "instability; admission-gate catchable)"),
      ADD_STAT(lvWrongAddrSame, statistics::units::Count::get(),
               "Wrong last-value forwards at the same line as the "
               "previous instance (value oscillation)"),
      ADD_STAT(lvCorrectAddrChanged, statistics::units::Count::get(),
               "Correct last-value forwards despite a line change "
               "(coincidental value match across addresses)"),
      ADD_STAT(lvCorrectAddrSame, statistics::units::Count::get(),
               "Correct last-value forwards at a stable line"),
      ADD_STAT(lvStrikes, statistics::units::Count::get(),
               "Second address-instability strikes that disabled a "
               "binding's last-value consumption (sticky, until "
               "sustained address stability)"),
      ADD_STAT(lvProbationSuppressed, statistics::units::Count::get(),
               "Confident last-value consumptions skipped while their "
               "binding was disabled by strikes")
{
    const int num_reasons = static_cast<int>(MrnSquashReason::Num);

    predictionsSquashedValue.init(num_reasons).flags(statistics::total);
    predictionsSquashedAlias.init(num_reasons).flags(statistics::total);

    for (int i = 0; i < num_reasons; i++) {
        predictionsSquashedValue.subname(i, mrnSquashReasonNames[i]);
        predictionsSquashedAlias.subname(i, mrnSquashReasonNames[i]);
    }

    static const char *vf_mode_names[] = {"alias", "producerValue",
                                          "lastValue"};
    vfPredictMade.init(MemRenamePredictor::VfModeCount)
        .flags(statistics::total);
    vfPredictCorrect.init(MemRenamePredictor::VfModeCount)
        .flags(statistics::total);
    vfPredictWrong.init(MemRenamePredictor::VfModeCount)
        .flags(statistics::total);
    vfPredictSquashed.init(MemRenamePredictor::VfModeCount)
        .flags(statistics::total);
    for (int i = 0; i < MemRenamePredictor::VfModeCount; i++) {
        vfPredictMade.subname(i, vf_mode_names[i]);
        vfPredictCorrect.subname(i, vf_mode_names[i]);
        vfPredictWrong.subname(i, vf_mode_names[i]);
        vfPredictSquashed.subname(i, vf_mode_names[i]);
    }
}

} // namespace o3
} // namespace gem5
