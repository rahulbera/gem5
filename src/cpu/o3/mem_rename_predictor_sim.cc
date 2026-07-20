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
      _predictIntLoadsOnly(p.predictIntLoadsOnly),
      _unified(p.mrnMode == enums::unified),
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
               "High-confidence loads forwarded via the mode-B value "
               "snapshot"),
      ADD_STAT(forwardsAlias, statistics::units::Count::get(),
               "High-confidence loads aliased to an in-flight producer's "
               "physreg (mode C)"),
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
      ADD_STAT(predictionsSquashedValue, statistics::units::Count::get(),
               "Mode-B (value) forwards squashed before they could verify, "
               "by squash reason"),
      ADD_STAT(predictionsSquashedAlias, statistics::units::Count::get(),
               "Mode-C (alias) forwards squashed before they could verify, "
               "by squash reason")
{
    const int num_reasons = static_cast<int>(MrnSquashReason::Num);

    predictionsSquashedValue.init(num_reasons).flags(statistics::total);
    predictionsSquashedAlias.init(num_reasons).flags(statistics::total);

    for (int i = 0; i < num_reasons; i++) {
        predictionsSquashedValue.subname(i, mrnSquashReasonNames[i]);
        predictionsSquashedAlias.subname(i, mrnSquashReasonNames[i]);
    }
}

} // namespace o3
} // namespace gem5
