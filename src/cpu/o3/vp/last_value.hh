#ifndef __CPU_O3_VP_LAST_VALUE_HH__
#define __CPU_O3_VP_LAST_VALUE_HH__

#include <optional>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/o3/vp/base.hh"
#include "cpu/o3/vp/lvp_table.hh"

namespace gem5
{

struct LastValueVPParams;

namespace o3
{

/**
 * Garfield VP: the Last-Value Predictor (Lipasti & Shen, MICRO-29 1996)
 * -- deliberately minimal, existing to prove the VP framework
 * end-to-end. A thin SimObject wrapper over the params-free LvpTable
 * (lvp_table.hh), plus table-level stats.
 */
class LastValueVP : public BaseValuePredictor
{
  public:
    LastValueVP(const LastValueVPParams &p);

  protected:
    std::optional<RegVal> predictImpl(Addr key) override;
    void trainImpl(Addr key, RegVal actualValue) override;

  private:
    LvpTable table;

    struct LvpStats : public statistics::Group
    {
        explicit LvpStats(statistics::Group *parent);
        statistics::Scalar lookups;
        statistics::Scalar hits;
        /** Hits below the confidence threshold (no prediction). */
        statistics::Scalar belowThreshold;
        statistics::Scalar allocs;
        /** Allocations that displaced a valid entry (subset of allocs;
         *  the table-thrash signal). */
        statistics::Scalar evictions;
        statistics::Scalar confResets;
        statistics::Scalar confDecrements;
    } lvpStats;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_VP_LAST_VALUE_HH__
