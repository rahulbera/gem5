#ifndef __CPU_O3_VP_VP_HISTORY_HH__
#define __CPU_O3_VP_VP_HISTORY_HH__

#include <cstdint>

#include "base/types.hh"

namespace gem5
{
namespace o3
{

/**
 * VTAGE-class predictors' only speculative state: a global
 * branch-direction history register and a path-history register
 * (design doc docs/superpowers/specs/2026-08-03-vtage-design.md,
 * "History Subsystem"). Maintained by the framework at fetch and
 * snapshotted onto every DynInst; params-free so it sits alongside
 * VtageTables and can be unit-tested without any SimObject/params
 * machinery.
 */
struct VpHistSnapshot
{
    /** Branch-direction history; bit 0 is the newest shifted-in
     *  direction. 64 bits wide -- VTAGE's longest configured history
     *  length (docs/superpowers/specs/2026-08-03-vtage-design.md,
     *  "Data Structures"). */
    uint64_t ghr = 0;
    /** Path history: low target-PC bits of taken control transfers. */
    uint16_t path = 0;
};

/**
 * Fetch-side history register pair. `branchShift`/`takenTarget` push
 * new history onto the (speculative) predicted path; `restore` snaps
 * it back to a prior snapshot on any fetch redirect. Restore-site
 * rules are per-initiator and live with their call sites (framework
 * spec, "History Subsystem"), not here.
 */
struct VpHistory
{
    VpHistSnapshot state;

    /** Shift a conditional branch's predicted direction into ghr. */
    void
    branchShift(bool taken)
    {
        state.ghr = (state.ghr << 1) | taken;
    }

    /** Shift a taken control transfer's low target-PC bits into path,
     *  masked to pathBits wide. */
    void
    takenTarget(Addr target, unsigned pathBits)
    {
        state.path =
            ((state.path << 3) | ((target >> 2) & 7)) & ((1u << pathBits) - 1);
    }

    /** Restore to a prior snapshot (fetch redirect). */
    void
    restore(const VpHistSnapshot &s)
    {
        state = s;
    }
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_VP_VP_HISTORY_HH__
