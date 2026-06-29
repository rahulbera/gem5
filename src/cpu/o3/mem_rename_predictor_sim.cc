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
      stats(this)
{}

MemRenamePredictor::MemRenameStats::MemRenameStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(storesTrained, statistics::units::Count::get(),
               "Stores that deposited a value into the MRN value file"),
      ADD_STAT(loadsTrained, statistics::units::Count::get(),
               "Loads that trained the MRN predictor at commit")
{}

} // namespace o3
} // namespace gem5
