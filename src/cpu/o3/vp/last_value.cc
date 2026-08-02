#include "cpu/o3/vp/last_value.hh"

#include "params/LastValueVP.hh"

namespace gem5
{
namespace o3
{

LastValueVP::LastValueVP(const LastValueVPParams &p)
    : BaseValuePredictor(p),
      table(LvpConfig{p.entries, p.assoc, p.confBits, p.confThreshold,
                      p.confDecrementOnWrong}),
      lvpStats(this)
{}

std::optional<RegVal>
LastValueVP::predictImpl(Addr key)
{
    lvpStats.lookups++;
    LvpLookup r = table.lookup(key);
    if (!r.hit) {
        return std::nullopt;
    }
    lvpStats.hits++;
    if (!r.confident) {
        lvpStats.belowThreshold++;
        return std::nullopt;
    }
    return r.value;
}

void
LastValueVP::trainImpl(Addr key, RegVal actualValue)
{
    switch (table.train(key, actualValue)) {
      case LvpTrainOutcome::Allocated:
        lvpStats.allocs++;
        break;
      case LvpTrainOutcome::Evicted:
        lvpStats.allocs++;
        lvpStats.evictions++;
        break;
      case LvpTrainOutcome::MismatchReset:
        lvpStats.confResets++;
        break;
      case LvpTrainOutcome::MismatchDecrement:
        lvpStats.confDecrements++;
        break;
      case LvpTrainOutcome::Match:
        break;
    }
}

LastValueVP::LvpStats::LvpStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "VPT lookups from in-scope predict() calls"),
      ADD_STAT(hits, statistics::units::Count::get(), "VPT tag hits"),
      ADD_STAT(belowThreshold, statistics::units::Count::get(),
               "VPT hits below the confidence threshold (no prediction)"),
      ADD_STAT(allocs, statistics::units::Count::get(),
               "VPT allocations (train misses)"),
      ADD_STAT(evictions, statistics::units::Count::get(),
               "VPT allocations that displaced a valid entry (subset "
               "of allocs; the table-thrash signal)"),
      ADD_STAT(confResets, statistics::units::Count::get(),
               "Confidence resets on a value mismatch"),
      ADD_STAT(confDecrements, statistics::units::Count::get(),
               "Confidence decrements on a value mismatch (decrement "
               "mode)")
{}

} // namespace o3
} // namespace gem5
