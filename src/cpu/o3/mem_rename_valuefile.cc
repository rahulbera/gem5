#include "cpu/o3/mem_rename_valuefile.hh"

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

/** floor(log2(bytes)): the largest shift such that (1u << shift) <=
 *  bytes. Inputs of 0 or 1 both floor to 0 (byte granularity); a
 *  non-power-of-two input floors down to its next lower power of two
 *  rather than falling back to 0 outright. */
inline unsigned
log2OfGranularity(unsigned bytes)
{
    unsigned shift = 0;
    while ((1u << (shift + 1)) <= bytes) {
        ++shift;
    }
    return shift;
}

} // anonymous namespace

MrnValueFileTables::MrnValueFileTables(const MrnVfConfig &cfg)
    : slcSets(numSets(cfg.slcEntries, cfg.slcAssoc)),
      slcAssoc(atLeastOne(cfg.slcAssoc)),
      scSets(numSets(cfg.scEntries, cfg.scAssoc)),
      scAssoc(atLeastOne(cfg.scAssoc)),
      scGranularityLog2(log2OfGranularity(cfg.scGranularityBytes)),
      confBits(cfg.confBits),
      confThreshold(cfg.confThreshold),
      confInc(cfg.confInc),
      confDec(cfg.confDec),
      resetConfOnMispredict(cfg.resetConfOnMispredict),
      valueFile(atLeastOne(cfg.vfEntries)),
      storeLoadCache(slcSets * slcAssoc),
      storeCache(scSets * scAssoc),
      lvStabilityTarget(cfg.lvStabilityTarget)
{}

MrnValueFileTables::SlcEntry *
MrnValueFileTables::slcFind(Addr pc)
{
    const unsigned set = static_cast<unsigned>(pc % slcSets);
    const unsigned firstWay = set * slcAssoc;
    for (unsigned w = 0; w < slcAssoc; ++w) {
        SlcEntry &e = storeLoadCache[firstWay + w];
        if (e.valid && e.tag == pc) {
            return &e;
        }
    }
    return nullptr;
}

MrnValueFileTables::SlcEntry *
MrnValueFileTables::slcAllocate(Addr pc)
{
    const unsigned set = static_cast<unsigned>(pc % slcSets);
    const unsigned firstWay = set * slcAssoc;
    SlcEntry *victim = &storeLoadCache[firstWay];
    for (unsigned w = 0; w < slcAssoc; ++w) {
        SlcEntry &e = storeLoadCache[firstWay + w];
        if (!e.valid) {
            victim = &e;
            break;
        }
        if (e.lru < victim->lru) {
            victim = &e;
        }
    }
    victim->valid = true;
    victim->tag = pc;
    victim->vfIdx = -1;
    victim->gen = 0;
    victim->conf = 0;
    victim->selfBound = false;
    return victim;
}

MrnValueFileTables::ScEntry *
MrnValueFileTables::scFind(Addr lineAddr)
{
    const unsigned set = static_cast<unsigned>(lineAddr % scSets);
    const unsigned firstWay = set * scAssoc;
    for (unsigned w = 0; w < scAssoc; ++w) {
        ScEntry &e = storeCache[firstWay + w];
        if (e.valid && e.tag == lineAddr) {
            return &e;
        }
    }
    return nullptr;
}

MrnValueFileTables::ScEntry *
MrnValueFileTables::scAllocate(Addr lineAddr)
{
    const unsigned set = static_cast<unsigned>(lineAddr % scSets);
    const unsigned firstWay = set * scAssoc;
    ScEntry *victim = &storeCache[firstWay];
    for (unsigned w = 0; w < scAssoc; ++w) {
        ScEntry &e = storeCache[firstWay + w];
        if (!e.valid) {
            victim = &e;
            break;
        }
        if (e.lru < victim->lru) {
            victim = &e;
        }
    }
    victim->valid = true;
    victim->tag = lineAddr;
    victim->vfIdx = -1;
    victim->gen = 0;
    victim->storeSeq = 0;
    return victim;
}

int
MrnValueFileTables::vfAllocate()
{
    int victim = 0;
    for (int i = 1; i < static_cast<int>(valueFile.size()); ++i) {
        if (valueFile[i].lru < valueFile[victim].lru) {
            victim = i;
        }
    }
    VfCell &cell = valueFile[victim];
    cell.gen++;
    cell.ptrValid = false;
    cell.ptr = nullptr;
    cell.ptrSeq = 0;
    cell.valueValid = false;
    cell.value = 0;
    return victim;
}

MrnVfRef
MrnValueFileTables::storeRename(Addr storePC, PhysRegIdPtr dataReg,
                                InstSeqNum sn, const RegVal *readyValue)
{
    SlcEntry *e = slcFind(storePC);
    if (!e) {
        e = slcAllocate(storePC);
        const int idx = vfAllocate();
        e->vfIdx = idx;
        e->gen = valueFile[idx].gen;
    } else if (valueFile[e->vfIdx].gen != e->gen) {
        // The cell was stolen since this store's entry last pointed at it:
        // grab a fresh cell rather than depositing into someone else's.
        const int idx = vfAllocate();
        e->vfIdx = idx;
        e->gen = valueFile[idx].gen;
    }

    VfCell &cell = valueFile[e->vfIdx];
    cell.ptr = dataReg;
    cell.ptrSeq = sn;
    cell.ptrValid = true;
    cell.valueValid = (readyValue != nullptr);
    cell.value = (readyValue != nullptr) ? *readyValue : 0;
    cell.lru = ++lruTick;
    e->lru = ++lruTick;

    return MrnVfRef{e->vfIdx, e->gen};
}

MrnVfCellRead
MrnValueFileTables::loadRename(Addr loadPC)
{
    MrnVfCellRead result;
    SlcEntry *e = slcFind(loadPC);
    if (!e || valueFile[e->vfIdx].gen != e->gen) {
        return result;
    }

    const VfCell &cell = valueFile[e->vfIdx];
    result.bound = true;
    result.confident = e->conf >= confThreshold;
    if (lvStabilityTarget && e->strikes >= 2) {
        result.lvProbation = true;
    }
    result.ptrValid = cell.ptrValid;
    result.ptr = cell.ptr;
    result.ptrSeq = cell.ptrSeq;
    result.valueValid = cell.valueValid;
    result.value = cell.value;
    result.ref = MrnVfRef{e->vfIdx, e->gen};
    return result;
}

bool
MrnValueFileTables::storeAddrResolved(const MrnVfRef &ref, Addr ea,
                                      InstSeqNum sn)
{
    if (!ref.valid() || valueFile[ref.idx].gen != ref.gen) {
        return false;
    }

    const Addr line = scLine(ea);
    ScEntry *e = scFind(line);
    if (e) {
        if (e->storeSeq > sn) {
            // The occupant is program-order younger than this publisher:
            // do not let an out-of-order-resolving older store clobber it.
            return false;
        }
    } else {
        e = scAllocate(line);
    }

    e->vfIdx = ref.idx;
    e->gen = ref.gen;
    e->storeSeq = sn;
    e->lru = ++lruTick;
    return true;
}

MrnVfProbeResult
MrnValueFileTables::loadAddrResolved(Addr loadPC, Addr ea)
{
    MrnVfProbeResult result;
    const Addr line = scLine(ea);
    ScEntry *sce = scFind(line);
    bool deadChannel = false;
    if (sce) {
        if (valueFile[sce->vfIdx].gen != sce->gen) {
            deadChannel = true;
            result.scHitDeadChannel = true;
        } else {
            sce->lru = ++lruTick;
        }
    }

    if (sce && !deadChannel) {
        SlcEntry *le = slcFind(loadPC);
        if (le && le->vfIdx == sce->vfIdx && le->gen == sce->gen) {
            le->lru = ++lruTick;
            result.outcome = MrnVfProbeResult::SameBinding;
            return result;
        }
        if (!le) {
            le = slcAllocate(loadPC);
        }
        le->vfIdx = sce->vfIdx;
        le->gen = sce->gen;
        le->conf = 0;
        le->selfBound = false;
        le->lru = ++lruTick;
        result.outcome = MrnVfProbeResult::Rebound;
        return result;
    }

    // Miss path (including a dead-channel SC hit).
    SlcEntry *le = slcFind(loadPC);
    if (le && le->selfBound && valueFile[le->vfIdx].gen == le->gen) {
        le->lru = ++lruTick;
        VfCell &cell = valueFile[le->vfIdx];
        const Addr line = scLine(ea);
        cell.addrChanged = cell.lineKnown && cell.lastLine != line;
        // Hysteresis re-enable: while last-value use is disabled by
        // strikes, count consecutive same-line executes; sustained
        // stability means the load's phase changed, so re-admit it.
        if (lvStabilityTarget && le->strikes >= 2) {
            if (cell.lineKnown && cell.lastLine == line) {
                if (++le->stableStreak >= lvStabilityTarget) {
                    le->strikes = 0;
                    le->stableStreak = 0;
                }
            } else {
                le->stableStreak = 0;
            }
        }
        cell.lastLine = line;
        cell.lineKnown = true;
        result.outcome = MrnVfProbeResult::AlreadySelfBound;
        return result;
    }

    const int idx = vfAllocate();
    if (!le) {
        le = slcAllocate(loadPC);
    }
    le->vfIdx = idx;
    le->gen = valueFile[idx].gen;
    le->conf = 0;
    le->selfBound = true;
    le->lru = ++lruTick;
    VfCell &fresh = valueFile[idx];
    fresh.lastLine = scLine(ea);
    fresh.lineKnown = true;
    fresh.addrChanged = false;
    result.outcome = MrnVfProbeResult::SelfBound;
    return result;
}

bool
MrnValueFileTables::loadDataResolved(Addr loadPC, RegVal value)
{
    SlcEntry *le = slcFind(loadPC);
    if (!le || !le->selfBound || valueFile[le->vfIdx].gen != le->gen) {
        return false;
    }

    VfCell &cell = valueFile[le->vfIdx];
    cell.value = value;
    cell.valueValid = true;
    cell.ptrValid = false;
    cell.lru = ++lruTick;
    return true;
}

void
MrnValueFileTables::trainVerify(Addr loadPC, const MrnVfRef &usedRef,
                                bool correct, bool addrChanged)
{
    lastDisable = false;
    SlcEntry *le = slcFind(loadPC);
    if (!le || le->vfIdx != usedRef.idx || le->gen != usedRef.gen) {
        return;
    }

    if (correct) {
        const unsigned next = le->conf + confInc;
        const unsigned maxConf = confMax();
        le->conf = (next > maxConf) ? maxConf : next;
        if (le->lvCorrects < 0xffffffffu) {
            le->lvCorrects++;
        }
        // Slow strike decay below the disable threshold: one strike per
        // 255 correct verifies (the disable criterion is therefore two
        // address-instability wrongs within ~255 corrects -- a DENSITY
        // test that spares sparse stumblers on high-value loads). An
        // address-change-tolerant load (hundreds of corrects per rare wrong)
        // sheds single strikes and never reaches the sticky disable; a churner
        // cannot decay between wrongs and trips it quickly. Disabled bindings
        // (strikes >= 2) re-enable only via observed address stability, never
        // by decay
        // -- oscillating regimes destabilize memory-dependence training.
        if (lvStabilityTarget && le->strikes == 1 &&
            ++le->strikeDecayCtr >= 255) {
            le->strikeDecayCtr = 0;
            le->strikes = 0;
        }
    } else if (resetConfOnMispredict) {
        le->conf = 0;
    } else {
        le->conf = (le->conf > confDec) ? (le->conf - confDec) : 0;
    }

    // Address-instability strike: a wrong forward whose address also
    // changed earns a strike; the second strike disables last-value
    // consumption for this PC until sustained address stability is
    // observed at execute (see loadAddrResolved). Value-oscillation
    // wrongs at a stable address keep the plain confidence-reset
    // behavior; alias and producer-value consumption are never gated.
    if (!correct && addrChanged && lvStabilityTarget) {
        if (le->lvWrongs < 0xffffu) {
            le->lvWrongs++;
        }
        if (le->strikes < 2) {
            le->strikes++;
        }
        le->stableStreak = 0;
        // Earning test: never disable a binding whose lifetime record
        // shows it pays for its rare wrongs. A burst of clustered wrongs
        // on a load earning hundreds of corrects per wrong (address-
        // tolerant, value-stable) must not tear down a schedule-critical
        // forwarding channel; a churner earning ~a hundred or less gets
        // disabled until its address stabilizes.
        const bool earns =
            le->lvWrongs >= 2 && (le->lvCorrects / le->lvWrongs) >= 256;
        if (earns && le->strikes >= 2) {
            le->strikes = 1; // hold one strike; do not disable
        }
        lastDisable = (le->strikes >= 2);
    }
}

bool
MrnValueFileTables::cellAddrChanged(const MrnVfRef &ref) const
{
    if (!ref.valid() || ref.idx >= static_cast<int>(valueFile.size())) {
        return false;
    }
    const VfCell &cell = valueFile[ref.idx];
    return cell.gen == ref.gen && cell.addrChanged;
}

} // namespace o3
} // namespace gem5
