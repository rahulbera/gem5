#ifndef __CPU_O3_GHOST_POLICY_HH__
#define __CPU_O3_GHOST_POLICY_HH__

namespace gem5
{
namespace o3
{

/**
 * Configuration for the ghost-execution policy. Stage 1 carries only the
 * master enable; later stages add per-class enables (value prediction,
 * memory renaming) without touching the datapath.
 */
struct GhostConfig
{
    bool enable = false;
};

/**
 * Decide whether a uop is ghost-executed. A ghost uop skips its OoO
 * issue-queue entry, issue-bandwidth slot, and execution port, while still
 * executing for correctness. Pure function so it is unit-testable in
 * isolation and so the policy stays the single place that grows per stage.
 *
 * Stage 1: a control uop is ghost when ghosting is enabled. Later stages
 * widen the inputs (isValuePredicted, isMemRenamed) and the GhostConfig.
 */
bool ghostPolicy(bool isControl, const GhostConfig &cfg);

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_GHOST_POLICY_HH__
