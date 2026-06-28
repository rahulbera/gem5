#include "cpu/o3/ghost_policy.hh"

namespace gem5
{
namespace o3
{

bool
ghostPolicy(bool isControl, const GhostConfig &cfg)
{
    return cfg.enable && isControl;
}

} // namespace o3
} // namespace gem5
