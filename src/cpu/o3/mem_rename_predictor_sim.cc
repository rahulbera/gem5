#include "cpu/o3/mem_rename_predictor.hh"
#include "params/MemRenamePredictor.hh"

namespace gem5
{
namespace o3
{

MemRenamePredictor::MemRenamePredictor(const MemRenamePredictorParams &p)
    : SimObject(p),
      tables(MrnConfig{p.storeTableEntries, p.storeTableAssoc,
                       p.loadTableEntries, p.loadTableAssoc,
                       p.valueFileEntries, p.confBits, p.confThreshold,
                       p.confInc, p.confDec, p.resetConfOnMispredict}),
      vfTables(MrnVfConfig{p.vfEntries, p.slcEntries, p.slcAssoc, p.scEntries,
                           p.scAssoc, p.scGranularityBytes, p.confBits,
                           p.confThreshold, p.confInc, p.confDec,
                           p.resetConfOnMispredict}),
      _aliasRequireCurrentProducer(p.aliasRequireCurrentProducer),
      _trainOnSnapshot(p.trainOnRenameSnapshot),
      _predictIntLoadsOnly(p.predictIntLoadsOnly),
      _enableValueForwarding(p.enableValueForwarding),
      _enableProducerAliasing(p.enableProducerAliasing),
      _useStoreSet(p.mrnCorrelation == enums::store_set),
      _useValueFile(p.mrnCorrelation == enums::value_file),
      _vfForwardProducerValue(p.vfForwardProducerValue),
      _vfForwardLastValue(p.vfForwardLastValue),
      stats(this)
{}

MemRenamePredictor::MemRenameStats::MemRenameStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(storesTrained, statistics::units::Count::get(),
               "Stores that deposited a value into the MRN value file"),
      ADD_STAT(loadsTrained, statistics::units::Count::get(),
               "Loads that trained the MRN predictor at commit"),
      ADD_STAT(predictLookups, statistics::units::Count::get(),
               "Loads looked up in the MRN predictor at rename"),
      ADD_STAT(predictionsMade, statistics::units::Count::get(),
               "High-confidence MRN predictions forwarded into a renamed reg"),
      ADD_STAT(predictionsCorrect, statistics::units::Count::get(),
               "Forwarded MRN loads that verified correct at writeback"),
      ADD_STAT(mispredicts, statistics::units::Count::get(),
               "Forwarded MRN loads that verified wrong and forced a squash"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Instructions discarded by MRN misprediction squashes"),
      ADD_STAT(forwardsValue, statistics::units::Count::get(),
               "High-confidence loads forwarded via the value snapshot "
               "snapshot"),
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
      ADD_STAT(bindingsLearned, statistics::units::Count::get(),
               "loadPC->storePC correlator bindings learned from LSQ "
               "forwarding"),
      ADD_STAT(producerCoverageCorrect, statistics::units::Count::get(),
               "COVERAGE: LSQ forwards where the correlator's binding matched "
               "the store the load actually forwarded from"),
      ADD_STAT(producerCoverageWrong, statistics::units::Count::get(),
               "COVERAGE: LSQ forwards where the correlator's binding named a "
               "different store PC than the actual producer"),
      ADD_STAT(producerCoverageUntrained, statistics::units::Count::get(),
               "COVERAGE: LSQ forwards where the correlator had no binding "
               "yet for this load PC"),
      ADD_STAT(producerPredictMade, statistics::units::Count::get(),
               "ACCURACY denominator: producer-PC predictions the correlator "
               "made at rename"),
      ADD_STAT(producerPredictCorrect, statistics::units::Count::get(),
               "ACCURACY: made predictions where the load actually forwarded "
               "from the predicted store PC"),
      ADD_STAT(producerPredictWrong, statistics::units::Count::get(),
               "ACCURACY: made predictions where the load did not forward "
               "from the predicted store PC"),
      ADD_STAT(producerPredictSquashed, statistics::units::Count::get(),
               "ACCURACY: made predictions whose load was squashed before "
               "writeback could validate it (made = correct+wrong+squashed)"),
      ADD_STAT(predictionsSquashedValue, statistics::units::Count::get(),
               "Value-path forwards squashed before they could verify, "
               "by squash reason"),
      ADD_STAT(predictionsSquashedAlias, statistics::units::Count::get(),
               "Alias-path forwards squashed before they could verify, "
               "by squash reason"),
      ADD_STAT(aliasProducerCurrent, statistics::units::Count::get(),
               "Alias attempts whose rename-map lookup matched the located "
               "store's own captured data physreg"),
      ADD_STAT(aliasProducerStale, statistics::units::Count::get(),
               "Alias attempts where the data arch reg was redefined between "
               "the located store's rename and the load's rename"),
      ADD_STAT(aliasNoInflightStore, statistics::units::Count::get(),
               "Made predictions abandoned: no usable in-flight store with "
               "the predicted PC in the store queue"),
      ADD_STAT(aliasStoreDataNotInt, statistics::units::Count::get(),
               "Made predictions abandoned: the located store's data source "
               "register was not integer"),
      ADD_STAT(aliasProducerUnusable, statistics::units::Count::get(),
               "Made predictions abandoned: the producer physreg was invalid, "
               "fixed-mapping, or already dead"),
      ADD_STAT(aliasStaleRejected, statistics::units::Count::get(),
               "Made predictions abandoned: the producer was stale and "
               "aliasRequireCurrentProducer refused the alias"),
      ADD_STAT(aliasOutcomeByStaleness, statistics::units::Count::get(),
               "Alias verify outcome bucketed by producer staleness"),
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
      ADD_STAT(vfShadowCorrect, statistics::units::Count::get(),
               "Value-file shadow comparisons that matched the true load "
               "value"),
      ADD_STAT(vfShadowWrong, statistics::units::Count::get(),
               "Value-file shadow comparisons that did not match the "
               "true load value"),
      ADD_STAT(vfShadowSkipped, statistics::units::Count::get(),
               "Value-file shadow comparisons skipped because the "
               "producer value was not available")
{
    const int num_reasons = static_cast<int>(MrnSquashReason::Num);

    static const char *staleness_names[] = {"currentWrong", "currentCorrect",
                                            "staleWrong", "staleCorrect"};
    aliasOutcomeByStaleness.init(4).flags(statistics::total);
    for (int i = 0; i < 4; i++) {
        aliasOutcomeByStaleness.subname(i, staleness_names[i]);
    }

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
