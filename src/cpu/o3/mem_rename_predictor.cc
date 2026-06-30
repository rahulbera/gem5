#include "cpu/o3/mem_rename_predictor.hh"

namespace gem5
{
namespace o3
{

namespace
{

/** Clamp a count to at least one so sizing/indexing never hits zero. */
inline unsigned
atLeastOne(unsigned v)
{
    return v == 0 ? 1 : v;
}

/** Number of sets for a set-associative table (rounded down, at least one). */
inline unsigned
numSets(unsigned entries, unsigned assoc)
{
    return atLeastOne(entries / atLeastOne(assoc));
}

/** Saturating maximum confidence value for the given counter width. */
inline unsigned
confMaxFromBits(unsigned bits)
{
    const unsigned capped = bits > 31 ? 31 : bits;
    return (1u << capped) - 1u;
}

} // anonymous namespace

MrnTables::MrnTables(const MrnConfig &cfg)
    : storeSets(numSets(cfg.storeTableEntries, cfg.storeTableAssoc)),
      storeAssoc(atLeastOne(cfg.storeTableAssoc)),
      loadSets(numSets(cfg.loadTableEntries, cfg.loadTableAssoc)),
      loadAssoc(atLeastOne(cfg.loadTableAssoc)),
      confThreshold(cfg.confThreshold),
      confInc(cfg.confInc),
      confDec(cfg.confDec),
      confMax(confMaxFromBits(cfg.confBits)),
      resetConfOnMispredict(cfg.resetConfOnMispredict),
      storeCache(storeSets * storeAssoc),
      loadCache(loadSets * loadAssoc),
      valueFile(atLeastOne(cfg.valueFileEntries)),
      fwdCache(loadSets * loadAssoc)
{}

MrnTables::StoreEntry *
MrnTables::storeFind(Addr addr)
{
    const unsigned set = static_cast<unsigned>(addr % storeSets);
    const unsigned firstWay = set * storeAssoc;
    for (unsigned w = 0; w < storeAssoc; ++w) {
        StoreEntry &e = storeCache[firstWay + w];
        if (e.valid && e.tag == addr) {
            return &e;
        }
    }
    return nullptr;
}

MrnTables::StoreEntry *
MrnTables::storeAllocate(Addr addr)
{
    const unsigned set = static_cast<unsigned>(addr % storeSets);
    const unsigned firstWay = set * storeAssoc;
    StoreEntry *victim = &storeCache[firstWay];
    for (unsigned w = 0; w < storeAssoc; ++w) {
        StoreEntry &e = storeCache[firstWay + w];
        if (!e.valid) {
            victim = &e;
            break;
        }
        if (e.lru < victim->lru) {
            victim = &e;
        }
    }
    victim->valid = true;
    victim->tag = addr;
    victim->slot = -1;
    return victim;
}

MrnTables::LoadEntry *
MrnTables::loadFind(Addr loadPC)
{
    const unsigned set = static_cast<unsigned>(loadPC % loadSets);
    const unsigned firstWay = set * loadAssoc;
    for (unsigned w = 0; w < loadAssoc; ++w) {
        LoadEntry &e = loadCache[firstWay + w];
        if (e.valid && e.tag == loadPC) {
            return &e;
        }
    }
    return nullptr;
}

MrnTables::LoadEntry *
MrnTables::loadAllocate(Addr loadPC)
{
    const unsigned set = static_cast<unsigned>(loadPC % loadSets);
    const unsigned firstWay = set * loadAssoc;
    LoadEntry *victim = &loadCache[firstWay];
    for (unsigned w = 0; w < loadAssoc; ++w) {
        LoadEntry &e = loadCache[firstWay + w];
        if (!e.valid) {
            victim = &e;
            break;
        }
        if (e.lru < victim->lru) {
            victim = &e;
        }
    }
    victim->valid = true;
    victim->tag = loadPC;
    victim->slot = -1;
    victim->conf = 0;
    return victim;
}

int
MrnTables::valueAllocate()
{
    int victim = 0;
    for (int i = 0; i < static_cast<int>(valueFile.size()); ++i) {
        if (!valueFile[i].valid) {
            return i;
        }
        if (valueFile[i].lru < valueFile[victim].lru) {
            victim = i;
        }
    }
    return victim;
}

void
MrnTables::commitStore(Addr effAddr, RegVal value)
{
    StoreEntry *e = storeFind(effAddr);
    if (!e) {
        e = storeAllocate(effAddr);
        e->slot = valueAllocate();
    }
    e->lru = ++lruTick;

    const int slot = e->slot;
    valueFile[slot].value = value;
    valueFile[slot].valid = true;
    valueFile[slot].lru = ++lruTick;
}

void
MrnTables::commitLoad(Addr loadPC, Addr effAddr, RegVal realValue, bool isSpGp)
{
    StoreEntry *se = storeFind(effAddr);
    if (!se) {
        // The load's address is not tracked by any store. Keep it simple and
        // leave the confidence unchanged (no decay).
        return;
    }
    const int slot = se->slot;
    se->lru = ++lruTick;

    // Bind loadPC -> slot in the load cache (allocate if new, conf starts 0).
    LoadEntry *le = loadFind(loadPC);
    if (!le) {
        le = loadAllocate(loadPC);
        le->slot = slot;
    } else if (le->slot != slot) {
        // Rebound to a different producer slot: retrain from scratch.
        le->slot = slot;
        le->conf = 0;
    }
    le->lru = ++lruTick;
    valueFile[slot].lru = ++lruTick;

    // Train the confidence counter against the snapshotted value.
    if (valueFile[slot].valid && valueFile[slot].value == realValue) {
        const unsigned inc = isSpGp ? (2 * confInc) : confInc;
        const unsigned next = le->conf + inc;
        le->conf = (next > confMax) ? confMax : next;
    } else if (resetConfOnMispredict) {
        le->conf = 0;
    } else {
        le->conf = (le->conf > confDec) ? (le->conf - confDec) : 0;
    }
}

MrnPrediction
MrnTables::predict(Addr loadPC)
{
    LoadEntry *le = loadFind(loadPC);
    if (le && le->conf >= confThreshold && le->slot >= 0 &&
        valueFile[le->slot].valid) {
        return MrnPrediction{true, valueFile[le->slot].value};
    }
    return MrnPrediction{false, 0};
}

void
MrnTables::mispredict(Addr loadPC)
{
    LoadEntry *le = loadFind(loadPC);
    if (le) {
        le->conf = 0;
    }
}

MrnTables::FwdEntry *
MrnTables::fwdFind(Addr loadPC)
{
    const unsigned set = static_cast<unsigned>(loadPC % loadSets);
    const unsigned firstWay = set * loadAssoc;
    for (unsigned w = 0; w < loadAssoc; ++w) {
        FwdEntry &e = fwdCache[firstWay + w];
        if (e.valid && e.tag == loadPC) {
            return &e;
        }
    }
    return nullptr;
}

MrnTables::FwdEntry *
MrnTables::fwdAllocate(Addr loadPC)
{
    const unsigned set = static_cast<unsigned>(loadPC % loadSets);
    const unsigned firstWay = set * loadAssoc;
    FwdEntry *victim = &fwdCache[firstWay];
    for (unsigned w = 0; w < loadAssoc; ++w) {
        FwdEntry &e = fwdCache[firstWay + w];
        if (!e.valid) {
            victim = &e;
            break;
        }
        if (e.lru < victim->lru) {
            victim = &e;
        }
    }
    victim->valid = true;
    victim->tag = loadPC;
    victim->storePC = 0;
    return victim;
}

void
MrnTables::trainForward(Addr loadPC, Addr storePC)
{
    FwdEntry *e = fwdFind(loadPC);
    if (!e) {
        e = fwdAllocate(loadPC);
    }
    e->storePC = storePC;
    e->lru = ++lruTick;
}

Addr
MrnTables::predictProducerPC(Addr loadPC)
{
    FwdEntry *e = fwdFind(loadPC);
    if (e && e->valid) {
        e->lru = ++lruTick;
        return e->storePC;
    }
    return 0;
}

} // namespace o3
} // namespace gem5
