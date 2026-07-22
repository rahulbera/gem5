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
      _aliasRequireCurrentProducer(p.aliasRequireCurrentProducer),
      _trainOnSnapshot(p.trainOnRenameSnapshot),
      _predictIntLoadsOnly(p.predictIntLoadsOnly),
      _enableValueForwarding(p.enableValueForwarding),
      _enableProducerAliasing(p.enableProducerAliasing),
      _useStoreSet(p.mrnCorrelation == enums::store_set),
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
      ADD_STAT(producerPredictCorrect, statistics::units::Count::get(),
               "LSQ forwards where the correlator's binding matched the "
               "store the load actually forwarded from"),
      ADD_STAT(producerPredictWrong, statistics::units::Count::get(),
               "LSQ forwards where the correlator's binding named a "
               "different store PC than the actual producer"),
      ADD_STAT(producerPredictUntrained, statistics::units::Count::get(),
               "LSQ forwards where the correlator had no binding yet for "
               "this load PC"),
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
      ADD_STAT(aliasOutcomeByStaleness, statistics::units::Count::get(),
               "Alias verify outcome bucketed by producer staleness")
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
}

} // namespace o3
} // namespace gem5
