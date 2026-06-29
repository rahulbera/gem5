#ifndef __CPU_O3_MEM_RENAME_PREDICTOR_HH__
#define __CPU_O3_MEM_RENAME_PREDICTOR_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "sim/sim_object.hh"

namespace gem5
{

// Forward declaration of the generated params struct. Keeping the SimObject
// constructor out-of-line (in mem_rename_predictor_sim.cc) lets the unit test
// exercise the plain-int core (MrnTables) without pulling in the generated
// params/MemRenamePredictor.hh header.
struct MemRenamePredictorParams;

namespace o3
{

/**
 * Result of a memory-renaming value prediction for a load. A high-confidence
 * hit returns valid=true with the snapshotted producer value.
 */
struct MrnPrediction
{
    bool valid;
    RegVal value;
};

/**
 * Plain-int configuration for MrnTables. Mirrors the SimObject params but is
 * free of any params/SimObject machinery, so the core logic can be built and
 * unit-tested in isolation.
 */
struct MrnConfig
{
    unsigned storeTableEntries = 1024;
    unsigned storeTableAssoc = 4;
    unsigned loadTableEntries = 1024;
    unsigned loadTableAssoc = 4;
    unsigned valueFileEntries = 512;
    unsigned confBits = 4;
    unsigned confThreshold = 8;
    unsigned confInc = 1;
    unsigned confDec = 1;
    bool resetConfOnMispredict = true;
};

/**
 * Core of the memory-renaming predictor (Garfield Stage 2, mode B = value
 * snapshot). Owns the three structures and all predict/train logic, and is
 * constructed purely from plain ints (MrnConfig) so it is unit-testable
 * without the SimObject/params machinery. MemRenamePredictor is a thin
 * SimObject wrapper that delegates to it.
 *
 *  - Store cache: effAddr -> value-file slot index. Set-associative with
 *                 modulo indexing and LRU replacement within a set.
 *  - Value file:  an independent vector of {value, valid} slots with LRU.
 *                 Kept separate from the store cache because a later mode (C)
 *                 repurposes these slots.
 *  - Load cache:  loadPC -> {value-file slot index, confidence}. Also
 *                 set-associative with modulo indexing and LRU.
 */
class MrnTables
{
  public:
    explicit MrnTables(const MrnConfig &cfg);

    /** Rename: return a high-confidence value prediction for a load PC. */
    MrnPrediction predict(Addr loadPC);

    /** Commit: a retiring store deposits its value for an address. */
    void commitStore(Addr effAddr, RegVal value);

    /** Commit: bind a load PC to the producer slot and train confidence. */
    void commitLoad(Addr loadPC, Addr effAddr, RegVal realValue, bool isSpGp);

    /** Writeback: reset a load PC's confidence (loop-safety). */
    void mispredict(Addr loadPC);

  private:
    struct StoreEntry
    {
        Addr tag = 0;
        int slot = -1;
        bool valid = false;
        uint64_t lru = 0;
    };

    struct LoadEntry
    {
        Addr tag = 0;
        int slot = -1;
        unsigned conf = 0;
        bool valid = false;
        uint64_t lru = 0;
    };

    struct ValueSlot
    {
        RegVal value = 0;
        bool valid = false;
        uint64_t lru = 0;
    };

    /** Find the store-cache entry for addr, or nullptr on a miss. */
    StoreEntry *storeFind(Addr addr);
    /** Allocate (LRU-evict within the set) a store-cache entry for addr. */
    StoreEntry *storeAllocate(Addr addr);
    /** Find the load-cache entry for loadPC, or nullptr on a miss. */
    LoadEntry *loadFind(Addr loadPC);
    /** Allocate (LRU-evict within the set) a load-cache entry for loadPC. */
    LoadEntry *loadAllocate(Addr loadPC);
    /** Allocate (LRU-evict) a value-file slot and return its index. */
    int valueAllocate();

    const unsigned storeSets;
    const unsigned storeAssoc;
    const unsigned loadSets;
    const unsigned loadAssoc;
    const unsigned confThreshold;
    const unsigned confInc;
    const unsigned confDec;
    const unsigned confMax;
    const bool resetConfOnMispredict;

    std::vector<StoreEntry> storeCache;
    std::vector<LoadEntry> loadCache;
    std::vector<ValueSlot> valueFile;

    /** Monotonic counter used as the LRU timestamp for all structures. */
    uint64_t lruTick = 0;
};

/**
 * Memory-renaming predictor SimObject. A thin wrapper that owns an MrnTables
 * and forwards each public operation to it; all behaviour lives in MrnTables
 * so it can be unit-tested without the params/SimObject machinery.
 */
class MemRenamePredictor : public SimObject
{
  public:
    MemRenamePredictor(const MemRenamePredictorParams &p);

    /** Rename: high-confidence value prediction for a load PC. */
    MrnPrediction
    predict(Addr loadPC)
    {
        return tables.predict(loadPC);
    }

    /** Commit: a retiring store deposits a value. */
    void
    commitStore(Addr effAddr, RegVal value)
    {
        tables.commitStore(effAddr, value);
    }

    /** Commit: bind the load to the producer slot and train confidence. */
    void
    commitLoad(Addr loadPC, Addr effAddr, RegVal realValue, bool isSpGp)
    {
        tables.commitLoad(loadPC, effAddr, realValue, isSpGp);
    }

    /** Writeback: reset confidence (loop-safety). */
    void
    mispredict(Addr loadPC)
    {
        tables.mispredict(loadPC);
    }

  private:
    MrnTables tables;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MEM_RENAME_PREDICTOR_HH__
