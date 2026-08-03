#include "cpu/o3/vp/evtage_tables.hh"

#include <cstdlib>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "cpu/o3/vp/vp_key.hh"

namespace gem5
{
namespace o3
{

EVtageTables::EVtageTables(const EVtageConfig &cfg,
                           std::function<double()> rng)
    : baseEntries(cfg.baseEntries),
      baseWayEntries(cfg.baseEntries / 2),
      taggedEntries(cfg.taggedEntries),
      numTagged(cfg.numTagged),
      historyLengths(cfg.historyLengths),
      baseTagBits(cfg.baseTagBits),
      confBits(cfg.confBits),
      confMax(cfg.confBits >= 32 ? ~0u : (1u << cfg.confBits) - 1),
      confThreshold(cfg.confThreshold),
      uBits(cfg.uBits),
      uMax(cfg.uBits >= 32 ? ~0u : (1u << cfg.uBits) - 1),
      tickMax(cfg.tickMax),
      burstGuardWindow(cfg.burstGuardWindow),
      pathBits(cfg.pathBits),
      logBaseWayEntries(cfg.baseEntries > 0 ? floorLog2(cfg.baseEntries / 2)
                                            : 0),
      logTaggedEntries(cfg.taggedEntries > 0 ? floorLog2(cfg.taggedEntries)
                                             : 0),
      indexFieldBits(std::max(logBaseWayEntries, logTaggedEntries)),
      rng(std::move(rng)),
      base(2, std::vector<Entry>(baseWayEntries)),
      tagged(numTagged, std::vector<Entry>(taggedEntries))
{
    fatal_if(cfg.baseEntries == 0 || (cfg.baseEntries % 2) != 0,
             "E-VTAGE baseEntries (%u) must be a nonzero even number "
             "(2 skewed ways, S5)",
             cfg.baseEntries);
    fatal_if(!isPowerOf2(baseWayEntries),
             "E-VTAGE baseEntries/2 (%u) must be a power of two",
             baseWayEntries);
    fatal_if(cfg.taggedEntries == 0 || !isPowerOf2(cfg.taggedEntries),
             "E-VTAGE taggedEntries (%u) must be a nonzero power of two",
             cfg.taggedEntries);
    fatal_if(cfg.numTagged < 1 || cfg.numTagged > 7,
             "E-VTAGE numTagged (%u) must be in [1, 7]: the biased-rank "
             "token field packs 0 = none, 1 = VT0, 2..(1+numTagged) = "
             "tagged components",
             cfg.numTagged);
    fatal_if(cfg.historyLengths.size() != cfg.numTagged,
             "E-VTAGE historyLengths has %zu entries, expected numTagged "
             "(%u)",
             cfg.historyLengths.size(), cfg.numTagged);
    for (unsigned length : cfg.historyLengths) {
        fatal_if(length == 0 || length > 128,
                 "E-VTAGE history length (%u) must be in [1, 128]: ghr "
                 "is a 128-bit (two-word) snapshot",
                 length);
    }
    fatal_if(cfg.confBits == 0 || cfg.confBits > 16,
             "E-VTAGE confBits (%u) must be in [1, 16]", cfg.confBits);
    fatal_if(cfg.confThreshold < 1,
             "E-VTAGE confThreshold must be >= 1: 0 predicts on every "
             "hit, so the corrective punish could never leave an entry "
             "below threshold (squash-livelock hazard)");
    fatal_if(cfg.confThreshold > confMax,
             "E-VTAGE confThreshold (%u) exceeds the %u-bit counter max "
             "(%u)",
             cfg.confThreshold, cfg.confBits, confMax);
    fatal_if(cfg.uBits == 0 || cfg.uBits > 16,
             "E-VTAGE uBits (%u) must be in [1, 16]", cfg.uBits);
    fatal_if(cfg.tickMax == 0,
             "E-VTAGE tickMax (%u) must be >= 1: 0 would trigger the "
             "aging pass on every allocation attempt",
             cfg.tickMax);
    fatal_if(cfg.pathBits == 0 || cfg.pathBits > 16,
             "E-VTAGE pathBits (%u) must be in [1, 16]: path is a "
             "16-bit snapshot register",
             cfg.pathBits);
    fatal_if(cfg.baseTagBits < 1, "E-VTAGE baseTagBits (%u) must be >= 1",
             cfg.baseTagBits);
    // The generalized F()-idiom base-way hash (fBankHash()) needs
    // regWidth > bank for its rotate math; the index hash's bank
    // constants are 1/2 (regWidth = logBaseWayEntries), the tag
    // hash's are 3/4/5/6 (regWidth = baseTagBits).
    fatal_if(logBaseWayEntries <= 2,
             "E-VTAGE baseEntries/2 (%u) needs log2 > 2 for the base-way "
             "index skew hash's bank constants",
             baseWayEntries);
    fatal_if(cfg.baseTagBits <= 6,
             "E-VTAGE baseTagBits (%u) needs > 6 for the base-way tag "
             "skew hash's bank constants",
             cfg.baseTagBits);
    fatal_if(cfg.numTagged >= logTaggedEntries,
             "E-VTAGE numTagged (%u) must be < log2(taggedEntries) (%u)",
             cfg.numTagged, logTaggedEntries);
    fatal_if(cfg.baseTagBits + cfg.numTagged > tagFieldBits,
             "E-VTAGE widest tag (baseTagBits %u + numTagged %u) exceeds "
             "the %u-bit token tag field",
             cfg.baseTagBits, cfg.numTagged, tagFieldBits);
    fatal_if(rankFieldBits + wayFieldBits + indexFieldBits + tagFieldBits >
                 64,
             "E-VTAGE token geometry does not fit 64 bits: rank %u + "
             "way %u + index %u + tag %u",
             rankFieldBits, wayFieldBits, indexFieldBits, tagFieldBits);
}

uint64_t
EVtageTables::shiftedPcOf(Addr pc, MicroPC upc) const
{
    // Same fold as VtageTables::shiftedPcOf() -- duplicated, not
    // shared, per the fork requirement (vtage_tables.{hh,cc} stay
    // byte-identical).
    return ((vpKey(pc, upc) >> 2) << 2) | (upc & 3);
}

uint64_t
EVtageTables::foldHistory(const VpHistSnapshot &h, unsigned origLength,
                          unsigned compLength) const
{
    if (compLength == 0) {
        return 0;
    }
    uint64_t comp = 0;
    for (unsigned i = 0; i < origLength; i++) {
        const uint64_t bit = histBit(h, origLength - 1 - i);
        comp = (comp << 1) | bit;
        comp ^= (comp >> compLength);
        comp &= (1ULL << compLength) - 1;
    }
    return comp;
}

uint64_t
EVtageTables::fBankHash(uint64_t path, unsigned size, unsigned regWidth,
                        unsigned bank) const
{
    uint64_t a = path & ((1ULL << size) - 1);
    const uint64_t a1 = a & ((1ULL << regWidth) - 1);
    uint64_t a2 = a >> regWidth;
    a2 = ((a2 << bank) & ((1ULL << regWidth) - 1)) +
         (a2 >> (regWidth - bank));
    a = a1 ^ a2;
    a = ((a << bank) & ((1ULL << regWidth) - 1)) +
        (a >> (regWidth - bank));
    return a;
}

unsigned
EVtageTables::computeTaggedIndex(uint64_t shiftedPc, const VpHistSnapshot &h,
                                 unsigned bank) const
{
    const unsigned length = historyLengths[bank - 1];
    const unsigned hlen = std::min(length, pathBits);
    const uint64_t idxFold = foldHistory(h, length, logTaggedEntries);
    const uint64_t pathFold = fBankHash(h.path, hlen, logTaggedEntries, bank);
    const int shiftAmt =
        std::abs(static_cast<int>(logTaggedEntries) - static_cast<int>(bank)) +
        1;
    const uint64_t index =
        shiftedPc ^ (shiftedPc >> shiftAmt) ^ idxFold ^ pathFold;
    return static_cast<unsigned>(index & ((1ULL << logTaggedEntries) - 1));
}

uint64_t
EVtageTables::computeTaggedTag(uint64_t shiftedPc, const VpHistSnapshot &h,
                               unsigned bank) const
{
    const unsigned length = historyLengths[bank - 1];
    const unsigned width = baseTagBits + bank;
    const uint64_t fold0 = foldHistory(h, length, width);
    const uint64_t fold1 = foldHistory(h, length, width - 1);
    const uint64_t tag = shiftedPc ^ fold0 ^ (fold1 << 1);
    return tag & ((1ULL << width) - 1);
}

unsigned
EVtageTables::computeBaseIndex(uint64_t shiftedPc, unsigned way) const
{
    const unsigned bank = way + 1; // 1 or 2: distinct from the tag
                                   // hash's bank constants below.
    const uint64_t h = fBankHash(shiftedPc, logBaseWayEntries,
                                 logBaseWayEntries, bank);
    const int shiftAmt = static_cast<int>(bank) + 1;
    const uint64_t index = shiftedPc ^ (shiftedPc >> shiftAmt) ^ h;
    return static_cast<unsigned>(index & (baseWayEntries - 1));
}

uint64_t
EVtageTables::computeBaseTag(uint64_t shiftedPc, unsigned way) const
{
    const unsigned bank0 = way + 3; // 3 or 4
    const unsigned bank1 = way + 5; // 5 or 6
    const uint64_t fold0 = fBankHash(shiftedPc, baseTagBits, baseTagBits,
                                     bank0);
    const uint64_t fold1 = fBankHash(shiftedPc, baseTagBits, baseTagBits,
                                     bank1);
    const uint64_t tag = shiftedPc ^ fold0 ^ (fold1 << 1);
    return tag & ((1ULL << baseTagBits) - 1);
}

EVtageTables::Provider
EVtageTables::findProvider(Addr pc, MicroPC upc,
                          const VpHistSnapshot &h) const
{
    const uint64_t shiftedPc = shiftedPcOf(pc, upc);
    for (unsigned bank = numTagged; bank > 0; bank--) {
        const unsigned index = computeTaggedIndex(shiftedPc, h, bank);
        const uint64_t tag = computeTaggedTag(shiftedPc, h, bank);
        const Entry &e = tagged[bank - 1][index];
        if (e.valid && e.tag == tag) {
            return Provider{true, bank, 0, index, tag};
        }
    }
    for (unsigned way = 0; way < 2; way++) {
        const unsigned index = computeBaseIndex(shiftedPc, way);
        const uint64_t tag = computeBaseTag(shiftedPc, way);
        const Entry &e = base[way][index];
        if (e.valid && e.tag == tag) {
            return Provider{true, 0, way, index, tag};
        }
    }
    return Provider{false, 0, 0, 0, 0};
}

uint64_t
EVtageTables::packToken(unsigned rank, unsigned way, unsigned index,
                        uint64_t tag) const
{
    const uint64_t rankField = static_cast<uint64_t>(rank) + 1;
    uint64_t token = rankField;
    token |= (static_cast<uint64_t>(way) & 0x1) << rankFieldBits;
    token |= static_cast<uint64_t>(index) << (rankFieldBits + wayFieldBits);
    token |= tag << (rankFieldBits + wayFieldBits + indexFieldBits);
    return token;
}

EVtageTables::UnpackedToken
EVtageTables::unpackToken(uint64_t token) const
{
    UnpackedToken u;
    u.rankField = token & ((1ULL << rankFieldBits) - 1);
    u.way = (token >> rankFieldBits) & ((1ULL << wayFieldBits) - 1);
    u.index = (token >> (rankFieldBits + wayFieldBits)) &
              ((1ULL << indexFieldBits) - 1);
    u.tag = (token >> (rankFieldBits + wayFieldBits + indexFieldBits)) &
            ((1ULL << tagFieldBits) - 1);
    return u;
}

EVtageLookup
EVtageTables::lookup(Addr pc, MicroPC upc, const VpHistSnapshot &h) const
{
    const Provider p = findProvider(pc, upc, h);
    EVtageLookup result;
    if (!p.found) {
        result.token = packToken(0, 0, 0, 0);
        return result;
    }
    result.hit = true;
    if (p.bank == 0) {
        const Entry &e = base[p.way][p.index];
        result.value = e.val;
        result.confident = e.c >= confThreshold;
        result.token = packToken(1, p.way, p.index, p.tag);
    } else {
        const Entry &e = tagged[p.bank - 1][p.index];
        result.value = e.val;
        result.confident = e.c >= confThreshold;
        result.token = packToken(p.bank + 1, 0, p.index, p.tag);
    }
    return result;
}

bool
EVtageTables::correctivePunish(uint64_t token)
{
    if (token == 0) {
        return false;
    }
    const UnpackedToken u = unpackToken(token);
    if (u.rankField == 0 || u.rankField == 1) {
        return false; // Absent, or a legal "no provider" token: nothing
                      // to punish.
    }
    Entry *ePtr = nullptr;
    if (u.rankField == 2) {
        if (u.way >= 2 || u.index >= baseWayEntries) {
            return false;
        }
        Entry &e = base[u.way][u.index];
        if (!e.valid || e.tag != u.tag) {
            return false;
        }
        ePtr = &e;
    } else {
        const unsigned bank = u.rankField - 2;
        if (bank > numTagged || u.index >= taggedEntries) {
            return false;
        }
        Entry &e = tagged[bank - 1][u.index];
        if (!e.valid || e.tag != u.tag) {
            return false;
        }
        ePtr = &e;
    }

    Entry &e = *ePtr;
    const unsigned cPre = e.c;
    const unsigned uPre = e.u;
    const bool medConf = (cPre > confMax / 2) ||
                         (cPre == confMax / 2 && uPre == uMax) ||
                         (cPre > 0 && cPre < confMax / 2);
    if (cPre == confMax) {
        e.u = 1;
        e.c = cPre - (confMax + 1) / 4;
    } else {
        e.c = 0;
        e.u = 0;
    }
    e.punishApplied = true;
    e.medConfPending = medConf;
    return true;
}

EVtageProviderPeek
EVtageTables::peekProvider(Addr pc, MicroPC upc, const VpHistSnapshot &h,
                          uint64_t token) const
{
    if (token != 0) {
        const UnpackedToken u = unpackToken(token);
        if (u.rankField == 1) {
            return EVtageProviderPeek{0, false, 0, 0};
        } else if (u.rankField == 2) {
            if (u.way < 2 && u.index < baseWayEntries) {
                const Entry &e = base[u.way][u.index];
                if (e.valid && e.tag == u.tag) {
                    return EVtageProviderPeek{1, e.c >= confMax, e.u, e.c};
                }
            }
        } else if (u.rankField >= 3) {
            const unsigned bank = u.rankField - 2;
            if (bank <= numTagged && u.index < taggedEntries) {
                const Entry &e = tagged[bank - 1][u.index];
                if (e.valid && e.tag == u.tag) {
                    return EVtageProviderPeek{bank + 1, e.c >= confMax, e.u,
                                             e.c};
                }
            }
        }
    }
    const Provider p = findProvider(pc, upc, h);
    if (!p.found) {
        return EVtageProviderPeek{0, false, 0, 0};
    }
    if (p.bank == 0) {
        const Entry &e = base[p.way][p.index];
        return EVtageProviderPeek{1, e.c >= confMax, e.u, e.c};
    }
    const Entry &e = tagged[p.bank - 1][p.index];
    return EVtageProviderPeek{p.bank + 1, e.c >= confMax, e.u, e.c};
}

bool
EVtageTables::burstGuardSuppresses(unsigned lastMispVT) const
{
    return burstGuardWindow > 0 && lastMispVT < burstGuardWindow;
}

std::pair<unsigned, uint64_t>
EVtageTables::debugBaseWayHash(Addr pc, MicroPC upc, unsigned way) const
{
    const uint64_t shiftedPc = shiftedPcOf(pc, upc);
    return {computeBaseIndex(shiftedPc, way), computeBaseTag(shiftedPc, way)};
}

unsigned
EVtageTables::lowVal(RegVal value)
{
    // CVP: abs(2*(int64_t)value + 1) < (1<<16), i.e. value (read as
    // signed) falls in [-32768, 32767]. `2*v + 1` in signed int64_t is
    // UB-prone at the extremes (signed overflow; negating INT64_MIN);
    // this well-defined unsigned window is equivalent for every
    // input: value + 32768 (mod 2^64) < 65536 iff value in
    // [-32768, 32767] as a signed 64-bit quantity.
    const uint64_t shifted = static_cast<uint64_t>(value) + 32768;
    unsigned result = (shifted < 65536) ? 1 : 0;
    if (value == 0) {
        result++;
    }
    return result;
}

bool
EVtageTables::fastInstBit(const EVtageClassifier &c)
{
    if (c.isLoad) {
        return c.memLevel == EVtageMemLevel::Stlf;
    }
    return !c.slowInst;
}

bool
EVtageTables::notL1Miss(const EVtageClassifier &c)
{
    if (c.isLoad) {
        return c.memLevel == EVtageMemLevel::Stlf ||
               c.memLevel == EVtageMemLevel::L1d;
    }
    return true;
}

bool
EVtageTables::notL2Miss(const EVtageClassifier &c)
{
    if (c.isLoad) {
        return c.memLevel != EVtageMemLevel::Mem;
    }
    return true;
}

bool
EVtageTables::notLlcMiss(const EVtageClassifier &c)
{
    if (c.isLoad) {
        return c.memLevel != EVtageMemLevel::Mem;
    }
    return true;
}

unsigned
EVtageTables::confidenceExponent(const EVtageClassifier &c)
{
    const unsigned lv = lowVal(c.value);
    const bool bracket = c.isLoad ? notL1Miss(c) : true;
    return lv + (notLlcMiss(c) ? 1 : 0) + (fastInstBit(c) ? 2 : 0) +
           (notL2Miss(c) ? 1 : 0) + (notL1Miss(c) ? 1 : 0) +
           (bracket ? 1 : 0);
}

unsigned
EVtageTables::allocationExponent(const EVtageClassifier &c)
{
    const unsigned lv = lowVal(c.value);
    const bool bracket = c.isLoad ? notL1Miss(c) : true;
    const unsigned bracketedLv = bracket ? lv : 0;
    return bracketedLv + (notLlcMiss(c) ? 1 : 0) + (notL2Miss(c) ? 1 : 0) +
           (notL1Miss(c) ? 1 : 0) + (fastInstBit(c) ? 2 : 0);
}

unsigned
EVtageTables::uExponent(const EVtageClassifier &c)
{
    const unsigned lv = lowVal(c.value);
    const bool aluBonus = !c.isLoad && c.fastInst && c.intSrcCount < 2;
    return lv + 2 * (notL1Miss(c) ? 1 : 0) + (c.isLoad ? 0 : 1) +
           (fastInstBit(c) ? 1 : 0) + (aluBonus ? 2 : 0);
}

bool
EVtageTables::maskFires(unsigned exponent)
{
    // C always evaluates random() as part of "(random() & mask) ==
    // 0", even when mask == 0 (exponent == 0) makes the comparison
    // unconditionally true -- one draw is always consumed here.
    const double draw = rng();
    if (exponent == 0) {
        return true;
    }
    const uint64_t scaled =
        static_cast<uint64_t>(draw * static_cast<double>(uint64_t{1}
                                                           << exponent));
    return scaled == 0;
}

bool
EVtageTables::confidenceGateFires(const EVtageClassifier &c,
                                  bool providerIsVt0)
{
    if (c.isIndirectCall) {
        return true;
    }
    const unsigned e = confidenceExponent(c);
    if (maskFires(e)) {
        return true;
    }
    if (providerIsVt0) {
        return maskFires(e); // K32 doubling: second independent trial.
    }
    return false;
}

bool
EVtageTables::updateUFires(const EVtageClassifier &c)
{
    if (c.isIndirectCall) {
        return true;
    }
    if (c.deliveredCorrect) {
        return false; // Short-circuit: no draw consumed.
    }
    return maskFires(uExponent(c));
}

bool
EVtageTables::shouldAllocate(const EVtageClassifier &c, bool medConf)
{
    if (c.isIndirectCall) {
        return true;
    }
    if (!c.isLoad && !c.slowInst) {
        // Outer operand gate (alu/store/undef only): one draw.
        const unsigned outerExponent = c.intSrcCount >= 2 ? 4 : 6;
        if (!maskFires(outerExponent)) {
            return false;
        }
    }
    // Body: always drawn, even when medConf is already true (C:
    // `draw() || MedConf` always evaluates the left operand). The
    // body's mask is `(2 << E') - 1` (design doc S3), i.e. exponent
    // E' + 1 -- one more halving than the confidence gate's `(1 <<
    // E) - 1` mask, NOT the same shape (verified worked endpoint:
    // DRAM-miss load, E' == 0, p == 1/2, not 1).
    const bool bodyFires = maskFires(allocationExponent(c) + 1);
    return bodyFires || medConf;
}

bool
EVtageTables::candidateQualifies(const Entry &e)
{
    if (e.u != 0) {
        return false;
    }
    if (e.c == confMax / 2) {
        return true;
    }
    const unsigned r = static_cast<unsigned>(rng() * (confMax + 1));
    const unsigned clamped = r > confMax ? confMax : r;
    return e.c <= clamped;
}

void
EVtageTables::tickUpdate(int na, int all,
                        std::vector<EVtageTrainOutcome> &outcomes)
{
    tick += na - 5 * all;
    if (tick < 0) {
        tick = 0;
    }
    if (tick >= static_cast<int>(tickMax)) {
        for (auto &way : base) {
            for (auto &e : way) {
                if (e.u > 0) {
                    e.u--;
                }
            }
        }
        for (auto &bankVec : tagged) {
            for (auto &e : bankVec) {
                if (e.u > 0) {
                    e.u--;
                }
            }
        }
        tick = 0;
        outcomes.push_back(EVtageTrainOutcome::TickPass);
    }
}

void
EVtageTables::landProviderExists(unsigned providerBankInternal, Addr pc,
                                MicroPC upc, const VpHistSnapshot &h,
                                RegVal actual,
                                const EVtageClassifier &classifier,
                                std::vector<EVtageTrainOutcome> &outcomes)
{
    // DEP, in our internal bank-index units, is exactly the spec's
    // "DEP = r + 1" (design doc S3): providerBankInternal == 0 (VT0,
    // either way) lands at internal bank 1 = VT1; a tagged provider
    // at internal bank k lands starting at k + 1 = VT(k+1). CVP's raw
    // source needs an *additional* "if (HitBank == 0) DEP++" bump
    // (mypredictor.cc's UpdateVtagePred) because CVP's own bank
    // numbering has TWO zero-history banks (0 and 1: mypredictor.h's
    // `HL[NHIST+1] = {0, 0, 3, 7, ...}`) that are themselves the two
    // skewed base ways -- CVP bank 1 (HitBank == 1, no bump) already
    // lands at DEP = 2 = CVP's first real tagged bank, so the bump
    // exists purely to bring bank 0 in line with bank 1, not to shift
    // the *tagged* target further out. Since this core already
    // collapses both base ways to providerBankInternal == 0 and
    // starts tagged numbering at our OWN internal bank 1 (one lower
    // than CVP's bank 2), no extra bump is needed here -- adding one
    // (an earlier, now-corrected version of this code did, reasoning
    // from a misread of CVP's HitBank numbering) silently made VT1
    // permanently unreachable.
    unsigned dep = providerBankInternal + 1;
    if (rng() < 1.0 / 8) {
        dep++;
    }

    const uint64_t shiftedPc = shiftedPcOf(pc, upc);
    int na = 0;
    int all = 0;
    for (unsigned bank = dep; bank <= numTagged; bank++) {
        const unsigned index = computeTaggedIndex(shiftedPc, h, bank);
        Entry &e = tagged[bank - 1][index];
        if (candidateQualifies(e)) {
            e.val = actual;
            e.c = confMax / 2;
            e.tag = computeTaggedTag(shiftedPc, h, bank);
            e.valid = true;
            e.u = 0;
            e.punishApplied = false;
            e.medConfPending = false;
            all++;
            outcomes.push_back(EVtageTrainOutcome::Allocated);
            break;
        }
        na++;
        outcomes.push_back(EVtageTrainOutcome::AllocScanStep);
    }
    tickUpdate(na, all, outcomes);
}

void
EVtageTables::landNoProvider(Addr pc, MicroPC upc, const VpHistSnapshot &h,
                            RegVal actual, const EVtageClassifier &classifier,
                            std::vector<EVtageTrainOutcome> &outcomes)
{
    const uint64_t shiftedPc = shiftedPcOf(pc, upc);
    int na = 0;
    int all = 0;
    if (rng() < 7.0 / 8) {
        const unsigned w = (rng() < 0.5) ? 0 : 1;
        for (unsigned j = 0; j < 2; j++) {
            const unsigned way = (w + j) & 1;
            const unsigned index = computeBaseIndex(shiftedPc, way);
            Entry &e = base[way][index];
            if (candidateQualifies(e)) {
                e.val = actual;
                e.c = confMax / 2;
                if (classifier.intSrcCount == 0 && classifier.fastInst &&
                    !classifier.isLoad) {
                    // Zero-operand-ALU special case (design doc S3):
                    // exists only in this base-way arm.
                    e.c = confMax;
                    outcomes.push_back(
                        EVtageTrainOutcome::AllocSeedSaturated);
                }
                e.tag = computeBaseTag(shiftedPc, way);
                e.valid = true;
                e.u = 0;
                e.punishApplied = false;
                e.medConfPending = false;
                all++;
                outcomes.push_back(EVtageTrainOutcome::AllocatedBaseWay);
                break;
            }
            na++;
            outcomes.push_back(EVtageTrainOutcome::AllocScanStep);
        }
    } else {
        // Internal bank 1 = VT1 = CVP's first real tagged bank (CVP
        // bank 2); see landProviderExists's doc comment for the full
        // CVP-bank-numbering derivation.
        unsigned dep = 1;
        if (rng() < 1.0 / 8) {
            dep++;
        }
        for (unsigned bank = dep; bank <= numTagged; bank++) {
            const unsigned index = computeTaggedIndex(shiftedPc, h, bank);
            Entry &e = tagged[bank - 1][index];
            if (candidateQualifies(e)) {
                e.val = actual;
                e.c = confMax / 2;
                e.tag = computeTaggedTag(shiftedPc, h, bank);
                e.valid = true;
                e.u = 0;
                e.punishApplied = false;
                e.medConfPending = false;
                all++;
                outcomes.push_back(EVtageTrainOutcome::Allocated);
                break;
            }
            na++;
            outcomes.push_back(EVtageTrainOutcome::AllocScanStep);
        }
    }
    tickUpdate(na, all, outcomes);
}

std::vector<EVtageTrainOutcome>
EVtageTables::train(Addr pc, MicroPC upc, const VpHistSnapshot &h,
                    uint64_t token, const EVtageClassifier &classifier)
{
    std::vector<EVtageTrainOutcome> outcomes;
    const UnpackedToken u = unpackToken(token);

    bool noProvider = false;
    bool stale = false;
    unsigned providerBank = 0; // 0 = VT0, 1..numTagged = tagged.
    unsigned providerWay = 0;
    unsigned providerIndex = 0;

    if (token == 0) {
        stale = true;
    } else if (u.rankField == 1) {
        noProvider = true;
    } else if (u.rankField == 2) {
        if (u.way >= 2 || u.index >= baseWayEntries ||
            !base[u.way][u.index].valid ||
            base[u.way][u.index].tag != u.tag) {
            stale = true;
        } else {
            providerBank = 0;
            providerWay = u.way;
            providerIndex = u.index;
        }
    } else {
        const unsigned bank = u.rankField - 2;
        if (bank > numTagged || u.index >= taggedEntries ||
            !tagged[bank - 1][u.index].valid ||
            tagged[bank - 1][u.index].tag != u.tag) {
            stale = true;
        } else {
            providerBank = bank;
            providerIndex = u.index;
        }
    }

    if (stale) {
        outcomes.push_back(EVtageTrainOutcome::StaleTokenRecomputed);
        const Provider p = findProvider(pc, upc, h);
        if (!p.found) {
            noProvider = true;
        } else {
            providerBank = p.bank;
            providerWay = p.way;
            providerIndex = p.index;
        }
    }

    if (noProvider) {
        outcomes.push_back(EVtageTrainOutcome::NoProviderTrained);
        if (shouldAllocate(classifier, /* medConf */ false)) {
            landNoProvider(pc, upc, h, classifier.value, classifier,
                          outcomes);
        }
        return outcomes;
    }

    Entry &entry = (providerBank == 0) ? base[providerWay][providerIndex]
                                       : tagged[providerBank - 1]
                                             [providerIndex];
    const bool correct = (entry.val == classifier.value);

    if (correct) {
        entry.punishApplied = false;
        entry.medConfPending = false;
        const bool providerIsVt0 = (providerBank == 0);
        if (entry.c < confMax) {
            if (confidenceGateFires(classifier, providerIsVt0)) {
                entry.c++;
                outcomes.push_back(EVtageTrainOutcome::CorrectInc);
            } else {
                outcomes.push_back(EVtageTrainOutcome::CorrectSat);
            }
        } else {
            outcomes.push_back(EVtageTrainOutcome::CorrectSat);
        }
        if (entry.u < uMax) {
            if (updateUFires(classifier) || entry.c == confMax) {
                entry.u++;
            }
        }
        return outcomes;
    }

    // Wrong.
    if (entry.punishApplied) {
        entry.val = classifier.value; // Unconditional overwrite.
        outcomes.push_back(EVtageTrainOutcome::WrongOverwriteOnly);
        const bool medConf = entry.medConfPending;
        entry.punishApplied = false;
        entry.medConfPending = false;
        if (shouldAllocate(classifier, medConf)) {
            landProviderExists(providerBank, pc, upc, h, classifier.value,
                              classifier, outcomes);
        }
    } else {
        const unsigned cPre = entry.c;
        const unsigned uPre = entry.u;
        const bool medConf = (cPre > confMax / 2) ||
                             (cPre == confMax / 2 && uPre == uMax) ||
                             (cPre > 0 && cPre < confMax / 2);
        entry.val = classifier.value; // Unconditional overwrite, first.
        if (cPre == confMax) {
            entry.u = 1;
            entry.c = cPre - (confMax + 1) / 4;
            outcomes.push_back(EVtageTrainOutcome::WrongPunishSat);
        } else {
            entry.c = 0;
            entry.u = 0;
            outcomes.push_back(EVtageTrainOutcome::WrongPunishReset);
        }
        entry.medConfPending = false;
        if (shouldAllocate(classifier, medConf)) {
            landProviderExists(providerBank, pc, upc, h, classifier.value,
                              classifier, outcomes);
        }
    }

    return outcomes;
}

} // namespace o3
} // namespace gem5
