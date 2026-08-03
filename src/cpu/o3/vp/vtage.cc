#include "cpu/o3/vp/vtage.hh"

#include <limits>
#include <string>

#include "base/cprintf.hh"
#include "params/VtageVP.hh"

namespace gem5
{
namespace o3
{

VtageVP::VtageVP(const VtageVPParams &p)
    : BaseValuePredictor(p),
      tableRng([this]() {
          return static_cast<double>(rng->random<uint32_t>()) /
                 (static_cast<double>(std::numeric_limits<uint32_t>::max()) +
                  1.0);
      }),
      table(VtageConfig{p.baseEntries, p.taggedEntries, p.numTagged,
                        p.historyLengths, p.baseTagBits, p.confBits,
                        p.confThreshold, p.fpcVector, p.historyPathBits},
            tableRng),
      vtageStats(this, 1 + p.numTagged)
{}

VpPredictResult
VtageVP::predictImpl(const VpLookupContext &ctx)
{
    const VtageLookup r = table.lookup(ctx.pc, ctx.upc, ctx.hist);
    const VtageProviderPeek peek =
        table.peekProvider(ctx.pc, ctx.upc, ctx.hist, r.token);
    vtageStats.providerLookups[peek.rank - 1]++;
    if (r.confident) {
        return {r.value, r.token};
    }
    // Below confidence: no value delivered, but the token is stamped
    // unconditionally (design doc, "Predict" step 3) so trainImpl()
    // still trains this lookup's provider at commit.
    return {std::nullopt, r.token};
}

void
VtageVP::trainImpl(const VpLookupContext &ctx, RegVal actualValue,
                   uint64_t token, const VpClassifierInfo &)
{
    // Peek the provider train() is about to resolve -- and whether
    // its confidence is already saturated -- before the mutating
    // call, so the outcomes below can be attributed to the right
    // per-component stat and the FPC fired/suppressed split can tell
    // "already saturated" apart from "attempted but not fired" (see
    // the fpcSuppressed doc comment, vtage.hh).
    const VtageProviderPeek peek =
        table.peekProvider(ctx.pc, ctx.upc, ctx.hist, token);
    const unsigned idx = peek.rank - 1;
    const auto outcomes =
        table.train(ctx.pc, ctx.upc, ctx.hist, token, actualValue);
    for (const VtageTrainOutcome outcome : outcomes) {
        switch (outcome) {
          case VtageTrainOutcome::CorrectInc:
            vtageStats.fpcFired++;
            vtageStats.providerCorrect[idx]++;
            break;
          case VtageTrainOutcome::CorrectSat:
            if (!peek.saturated) {
                vtageStats.fpcSuppressed++;
            }
            vtageStats.providerCorrect[idx]++;
            break;
          case VtageTrainOutcome::WrongReset:
          case VtageTrainOutcome::WrongValOverwrite:
            vtageStats.providerWrong[idx]++;
            break;
          case VtageTrainOutcome::Allocated:
            vtageStats.allocations++;
            break;
          case VtageTrainOutcome::AllocFailedAged:
            vtageStats.allocationFailures++;
            break;
          case VtageTrainOutcome::StaleTokenRecomputed:
            vtageStats.tokenStaleRecomputes++;
            break;
        }
    }
}

bool
VtageVP::correctiveResetImpl(uint64_t token)
{
    const bool live = table.correctiveReset(token);
    if (live) {
        vtageStats.correctiveResets++;
    }
    return live;
}

VtageVP::VtageStats::VtageStats(statistics::Group *parent,
                                unsigned numComponents)
    : statistics::Group(parent),
      ADD_STAT(providerLookups, statistics::units::Count::get(),
               "Rename-time lookups by resolved provider (includes "
               "below-threshold lookups that deliver no value)"),
      ADD_STAT(providerCorrect, statistics::units::Count::get(),
               "Commit-time trainings whose provider verified correct, "
               "by provider"),
      ADD_STAT(providerWrong, statistics::units::Count::get(),
               "Commit-time trainings whose provider verified wrong, "
               "by provider"),
      ADD_STAT(allocations, statistics::units::Count::get(),
               "Wrong-training allocations into a higher-rank "
               "component"),
      ADD_STAT(allocationFailures, statistics::units::Count::get(),
               "Wrong-training allocation probes that found no u == 0 "
               "candidate above the provider (u-aging fired instead)"),
      ADD_STAT(correctiveResets, statistics::units::Count::get(),
               "Verify-site corrective resets whose token still "
               "matched a live provider"),
      ADD_STAT(tokenStaleRecomputes, statistics::units::Count::get(),
               "Trainings whose token was stale or absent, recomputing "
               "the provider by longest match instead"),
      ADD_STAT(fpcFired, statistics::units::Count::get(),
               "FPC forward confidence transitions that fired on a "
               "correct update"),
      ADD_STAT(fpcSuppressed, statistics::units::Count::get(),
               "FPC forward confidence transitions attempted (counter "
               "below its saturating max) but suppressed by the gate, "
               "on a correct update")
{
    // Sized 1 (base) + numTagged: numTagged legally reaches 7, so a
    // hardcoded 7 would make biased rank 8 index out of bounds.
    providerLookups.init(numComponents).flags(statistics::total);
    providerCorrect.init(numComponents).flags(statistics::total);
    providerWrong.init(numComponents).flags(statistics::total);
    for (unsigned i = 0; i < numComponents; i++) {
        const std::string name = csprintf("vt%u", i);
        providerLookups.subname(i, name);
        providerCorrect.subname(i, name);
        providerWrong.subname(i, name);
    }
}

} // namespace o3
} // namespace gem5
