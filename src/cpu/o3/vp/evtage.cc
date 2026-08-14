#include "cpu/o3/vp/evtage.hh"

#include <limits>
#include <string>

#include "base/cprintf.hh"
#include "params/EVtageVP.hh"

namespace gem5
{
namespace o3
{

namespace
{

/** VpClassifierInfo::memSrcLevel -> EVtageMemLevel (design doc docs/
 *  superpowers/specs/2026-08-04-evtage-design.md, "Latency-classifier
 *  mapping"). Raw values are DynInst::MemSrcLevel's (MemSrcStlf=0/
 *  MemSrcL1D=1/MemSrcL2=2/MemSrcMem=3/MemSrcUnknown=4, dyn_inst.hh) --
 *  carried as a plain uint8_t through VpClassifierInfo so vp_types.hh
 *  stays free of a dyn_inst.hh dependency; MemSrcMem and the
 *  MemSrcUnknown fallback both map to EVtageMemLevel::Mem (our
 *  3-level hierarchy has no distinct LLC row -- evtage_tables.hh's
 *  EVtageMemLevel doc comment). */
EVtageMemLevel
toMemLevel(uint8_t raw)
{
    switch (raw) {
        case 0:
            return EVtageMemLevel::Stlf;
        case 1:
            return EVtageMemLevel::L1d;
        case 2:
            return EVtageMemLevel::L2;
        default:
            return EVtageMemLevel::Mem;
    }
}

/** VpClassifierInfo -> EVtageClassifier (design doc, "Framework API
 *  Changes"): the framework's class-agnostic classifier translated
 *  into the params-free core's own shape. */
EVtageClassifier
toEVtageClassifier(const VpClassifierInfo &c, RegVal actualValue)
{
    EVtageClassifier ec;
    ec.isLoad = (c.instClass == VpInstClass::Load);
    ec.memLevel = toMemLevel(c.memSrcLevel);
    ec.fastInst = (c.instClass == VpInstClass::Alu);
    ec.slowInst = (c.instClass == VpInstClass::SlowAlu);
    ec.intSrcCount = c.nbOperand;
    ec.isIndirectCall = (c.instClass == VpInstClass::IndirectCall);
    ec.value = actualValue;
    ec.deliveredCorrect = c.deliveredCorrect;
    return ec;
}

} // anonymous namespace

EVtageVP::EVtageVP(const EVtageVPParams &p)
    : BaseValuePredictor(p),
      tableRng([this]() {
          return static_cast<double>(rng->random<uint32_t>()) /
                 (static_cast<double>(std::numeric_limits<uint32_t>::max()) +
                  1.0);
      }),
      table(EVtageConfig{p.baseEntries, p.taggedEntries, p.numTagged,
                         p.historyLengths, p.baseTagBits, p.confBits,
                         p.confThreshold, p.uBits, p.tickMax,
                         p.burstGuardWindow, p.historyPathBits},
            tableRng),
      evtageStats(this, 2 + p.numTagged)
{
    // renamedInsts() is now fed once per renamed instruction (see
    // base.hh's notifyRenamedInst()), but this predictor's burst
    // guard stays locked off pending re-validation of a nonzero
    // window: the guard-capable configuration lives in the EVES
    // composition predictor. Fail loudly if set here regardless.
    fatal_if(p.burstGuardWindow > 0,
             "EVtageVP burstGuardWindow is locked off; the "
             "guard-capable configuration lives in the EVES "
             "composition predictor");
}

VpPredictResult
EVtageVP::predictImpl(const VpLookupContext &ctx)
{
    const EVtageLookup r = table.lookup(ctx.pc, ctx.upc, ctx.hist);
    const EVtageProviderPeek peek =
        table.peekProvider(ctx.pc, ctx.upc, ctx.hist, r.token);
    // Biased rank IS the vector index here (0 = none), unlike
    // VtageVP's peek.rank - 1 -- E-VTAGE's rank-0 "no provider" is a
    // legal outcome (design doc, "Rank-0 peek/stat contract").
    evtageStats.providerLookups[peek.rank]++;
    if (peek.rank == 0) {
        evtageStats.noProviderLookups++;
        evtageStats.baseTagMiss++;
    }
    if (!r.confident) {
        // Below confidence (or no provider): no value delivered, but
        // the token is stamped unconditionally so trainImpl() still
        // trains this lookup's provider at commit.
        return {std::nullopt, r.token};
    }
    const unsigned lastMispVT = renamedInsts(ctx.tid) - lastWrongMark[ctx.tid];
    if (table.burstGuardSuppresses(lastMispVT)) {
        // S6: emission suppressed, but the lookup ran and the token
        // is still stamped -- commit training proceeds unchanged.
        evtageStats.burstGuardSuppressed++;
        return {std::nullopt, r.token};
    }
    return {r.value, r.token};
}

void
EVtageVP::trainImpl(const VpLookupContext &ctx, RegVal actualValue,
                    uint64_t token, const VpClassifierInfo &classifier)
{
    // Peek the provider train() is about to resolve -- and whether
    // its confidence is already saturated -- before the mutating
    // call, same pattern as VtageVP (vtage.cc).
    const EVtageProviderPeek peek =
        table.peekProvider(ctx.pc, ctx.upc, ctx.hist, token);
    const unsigned idx = peek.rank;

    const EVtageClassifier ec = toEVtageClassifier(classifier, actualValue);
    const auto outcomes = table.train(ctx.pc, ctx.upc, ctx.hist, token, ec);

    bool sawCorrect = false;
    bool sawWrongOverwriteOnly = false;
    for (const EVtageTrainOutcome outcome : outcomes) {
        switch (outcome) {
            case EVtageTrainOutcome::CorrectInc:
                evtageStats.confGateFired++;
                evtageStats.providerCorrect[idx]++;
                sawCorrect = true;
                break;
            case EVtageTrainOutcome::CorrectSat:
                if (!peek.saturated) {
                    evtageStats.confGateSuppressed++;
                }
                evtageStats.providerCorrect[idx]++;
                sawCorrect = true;
                break;
            case EVtageTrainOutcome::WrongPunishSat:
            case EVtageTrainOutcome::WrongPunishReset:
                evtageStats.providerWrong[idx]++;
                break;
            case EVtageTrainOutcome::WrongOverwriteOnly:
                evtageStats.providerWrong[idx]++;
                sawWrongOverwriteOnly = true;
                break;
            case EVtageTrainOutcome::NoProviderTrained:
                evtageStats.noProviderTrains++;
                break;
            case EVtageTrainOutcome::StaleTokenRecomputed:
                evtageStats.tokenStaleRecomputes++;
                break;
            case EVtageTrainOutcome::AllocScanStep:
                evtageStats.allocScanSteps++;
                break;
            case EVtageTrainOutcome::Allocated:
                evtageStats.allocSteals++;
                break;
            case EVtageTrainOutcome::AllocatedBaseWay:
                evtageStats.allocSteals++;
                evtageStats.allocBaseWay++;
                break;
            case EVtageTrainOutcome::AllocSeedSaturated:
                evtageStats.allocSeedSaturated++;
                break;
            case EVtageTrainOutcome::TickPass:
                evtageStats.tickPasses++;
                break;
        }
    }

    // Pending-bit lifecycle bookkeeping (design doc S3): a
    // WrongOverwriteOnly outcome is the deferred remainder consuming
    // a prior verify-site punish; a correct outcome on a
    // still-pending token drops it instead (mutually exclusive with
    // the wrong branch within a single train() call).
    if (sawWrongOverwriteOnly) {
        if (pendingTokens.erase(token) > 0) {
            evtageStats.pendingConsumed++;
        }
    } else if (sawCorrect) {
        if (pendingTokens.erase(token) > 0) {
            evtageStats.pendingDropped++;
        }
    }
}

bool
EVtageVP::correctiveResetImpl(uint64_t token)
{
    const bool live = table.correctivePunish(token);
    if (live) {
        evtageStats.correctiveResets++;
        evtageStats.pendingSet++;
        pendingTokens.insert(token);
        // See lastWrongMark's doc comment (evtage.hh): correctiveReset
        // carries no thread ID, so every thread's mark resets together
        // rather than only the wronged thread's.
        for (ThreadID tid = 0; tid < MaxThreads; tid++) {
            lastWrongMark[tid] = renamedInsts(tid);
        }
    }
    return live;
}

EVtageVP::EVtageStats::EVtageStats(statistics::Group *parent,
                                   unsigned numComponents)
    : statistics::Group(parent),
      ADD_STAT(providerLookups, statistics::units::Count::get(),
               "Rename-time lookups by resolved provider, including "
               "an explicit none (rank 0, no provider) bucket"),
      ADD_STAT(providerCorrect, statistics::units::Count::get(),
               "Commit-time trainings whose provider verified correct, "
               "by provider"),
      ADD_STAT(providerWrong, statistics::units::Count::get(),
               "Commit-time trainings whose provider verified wrong, "
               "by provider"),
      ADD_STAT(confGateFired, statistics::units::Count::get(),
               "S1 classifier-driven confidence-gate forward "
               "transitions that fired on a correct update"),
      ADD_STAT(confGateSuppressed, statistics::units::Count::get(),
               "S1 classifier-driven confidence-gate transitions "
               "attempted (counter below its saturating max) but "
               "suppressed by the gate, on a correct update"),
      ADD_STAT(correctiveResets, statistics::units::Count::get(),
               "Verify-site corrective punishes whose token still "
               "matched a live provider"),
      ADD_STAT(tokenStaleRecomputes, statistics::units::Count::get(),
               "Trainings whose token was stale or absent, "
               "recomputing the provider by longest match instead"),
      ADD_STAT(baseTagMiss, statistics::units::Count::get(),
               "Rename-time lookups where neither VT0 way tag-"
               "matched (equals noProviderLookups: a tagged hit "
               "preempts the base check under this port's search "
               "order, so the two are indistinguishable)"),
      ADD_STAT(noProviderLookups, statistics::units::Count::get(),
               "Rename-time lookups that resolved to the legal "
               "rank-0 'no provider' outcome"),
      ADD_STAT(noProviderTrains, statistics::units::Count::get(),
               "Commit-time trainings whose token resolved to 'no "
               "provider'"),
      ADD_STAT(allocScanSteps, statistics::units::Count::get(),
               "S3 allocation-scan candidates visited and not stolen "
               "(NA, summed across every allocation attempt)"),
      ADD_STAT(allocSteals, statistics::units::Count::get(),
               "S3 allocation-scan candidates successfully stolen "
               "(ALL, summed; tagged-bank plus base-way steals)"),
      ADD_STAT(allocBaseWay, statistics::units::Count::get(),
               "Subset of allocSteals that landed in a VT0 base way"),
      ADD_STAT(allocSeedSaturated, statistics::units::Count::get(),
               "Zero-operand-ALU base-way steals seeded to confMax "
               "instead of the usual confMax/2"),
      ADD_STAT(tickPasses, statistics::units::Count::get(),
               "TICK aging passes (every entry's nonzero u "
               "decremented, all components including base ways)"),
      ADD_STAT(pendingSet, statistics::units::Count::get(),
               "Verify-site corrective punishes that set the "
               "deferred-remainder pending bits"),
      ADD_STAT(pendingConsumed, statistics::units::Count::get(),
               "Deferred commit-train remainders that consumed a "
               "pending mark"),
      ADD_STAT(pendingDropped, statistics::units::Count::get(),
               "Pending marks dropped by an intervening correct "
               "outcome before the deferred remainder ran"),
      ADD_STAT(burstGuardSuppressed, statistics::units::Count::get(),
               "S6 burst-guard: rename-time lookups whose emission "
               "was suppressed (zero when burstGuardWindow == 0)")
{
    // numComponents = 2 (none, VT0) + numTagged: numTagged legally
    // reaches 7, so a hardcoded 8 would make biased rank 8 (VT7) out
    // of bounds if it did not, and a hardcoded 2 would drop the none
    // bucket VtageVP never needed.
    providerLookups.init(numComponents).flags(statistics::total);
    providerCorrect.init(numComponents).flags(statistics::total);
    providerWrong.init(numComponents).flags(statistics::total);
    providerLookups.subname(0, "none");
    providerCorrect.subname(0, "none");
    providerWrong.subname(0, "none");
    for (unsigned i = 1; i < numComponents; i++) {
        const std::string name = csprintf("vt%u", i - 1);
        providerLookups.subname(i, name);
        providerCorrect.subname(i, name);
        providerWrong.subname(i, name);
    }
}

} // namespace o3
} // namespace gem5
