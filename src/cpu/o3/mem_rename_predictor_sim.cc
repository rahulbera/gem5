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
                       p.confInc, p.confDec, p.resetConfOnMispredict})
{}

} // namespace o3
} // namespace gem5
