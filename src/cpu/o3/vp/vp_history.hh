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

/**
 * Pure (non-mutating) version of branchShift()/takenTarget(), folded
 * into one call: compute the snapshot that results from applying one
 * control-flow event to `snap`, without touching any live
 * VpHistory/vpHist[tid] state. Commit uses this to precompute a
 * restore-carrier value (design doc, "History Subsystem", the
 * squash-after restore rule) for the fetch-side restoreHistory() call
 * to consume later -- keeping the single-writer timing invariant that
 * only fetch ever mutates the live history register, exactly when it
 * processes the squash.
 */
inline VpHistSnapshot
advanceSnapshot(const VpHistSnapshot &snap, bool isCond, bool taken,
                Addr target, unsigned pathBits)
{
    VpHistory hist{snap};
    if (isCond) {
        hist.branchShift(taken);
    }
    if (taken) {
        hist.takenTarget(target, pathBits);
    }
    return hist.state;
}

/**
 * Which pipeline redirect a commit-computed restore carrier
 * (CommitComm::vpHistRestore, comm.hh) is for. Branch/Decode
 * initiators restore via their existing DynInst-based paths (they
 * always have a live mispredictInst/squashInst to read a snapshot
 * off), so they have no carrier and no entry here; see
 * BaseValuePredictor::VpHistInitiator (base.hh) for the full
 * initiator set the historyRestores stat tracks.
 */
enum class VpHistRestoreKind
{
    Inclusive,
    Trap,
    SquashAfter
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_VP_VP_HISTORY_HH__
