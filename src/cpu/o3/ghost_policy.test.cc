#include <gtest/gtest.h>

#include "cpu/o3/ghost_policy.hh"

using namespace gem5::o3;

// Knob off: nothing is ever ghost, regardless of control-ness.
TEST(GhostPolicy, DisabledNeverGhosts)
{
    GhostConfig cfg; // enable defaults to false
    EXPECT_FALSE(ghostPolicy(/*isControl=*/true, cfg));
    EXPECT_FALSE(ghostPolicy(/*isControl=*/false, cfg));
}

// Knob on: exactly the control uops are ghost.
TEST(GhostPolicy, EnabledGhostsControlOnly)
{
    GhostConfig cfg;
    cfg.enable = true;
    EXPECT_TRUE(ghostPolicy(/*isControl=*/true, cfg));
    EXPECT_FALSE(ghostPolicy(/*isControl=*/false, cfg));
}
