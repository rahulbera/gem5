#ifndef __CPU_O3_VP_VP_TYPES_HH__
#define __CPU_O3_VP_VP_TYPES_HH__

#include <cstdint>
#include <optional>

#include "base/types.hh"
#include "cpu/o3/vp/vp_history.hh"

namespace gem5
{
namespace o3
{

/**
 * Everything predictImpl()/trainImpl() need to look an entry up: the
 * (PC, micro-PC) pair the framework already folds via vp_key.hh, plus
 * the fetch-time history snapshot (docs/superpowers/specs/2026-08-03-
 * vtage-design.md, "Framework API Changes"). LVP ignores hist and
 * folds pc/upc through vpKey() unchanged; VTAGE-class predictors fold
 * pc/upc with hist.{ghr,path} instead.
 */
struct VpLookupContext
{
    Addr pc;
    MicroPC upc;
    VpHistSnapshot hist;
};

/**
 * predictImpl()'s return: an optional value to consume, plus an opaque
 * provider token. The base class stamps the token on the instruction
 * on every lookup -- even when no value is delivered (a below-
 * confidence hit) -- so trainImpl()/correctiveReset() can recover the
 * exact provider without a second table search. LVP always returns
 * token 0 (it has no provider concept to recover).
 */
struct VpPredictResult
{
    std::optional<RegVal> value;
    uint64_t token = 0;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_VP_VP_TYPES_HH__
