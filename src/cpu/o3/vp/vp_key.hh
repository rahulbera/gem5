#ifndef __CPU_O3_VP_VP_KEY_HH__
#define __CPU_O3_VP_VP_KEY_HH__

#include "base/types.hh"

namespace gem5
{
namespace o3
{

/**
 * Fold an instruction's (PC, micro-PC) into the value-prediction lookup
 * key. Cracked macro-ops (e.g. an AArch64 ldp splitting into two
 * micro-ops) share a PC; without the micro-PC in the key their distinct
 * destination values would ping-pong one table entry and confidence
 * could never build. The micro-PC lands in the upper bits, away from
 * the set-index bits, so the micro-ops of one PC land in the same set
 * with distinct tags.
 */
static inline Addr
vpKey(Addr pc, MicroPC upc)
{
    return pc ^ (static_cast<Addr>(upc) << 48);
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_VP_VP_KEY_HH__
