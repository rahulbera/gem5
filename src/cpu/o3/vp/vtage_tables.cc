#include "cpu/o3/vp/vtage_tables.hh"

#include <algorithm>
#include <cstdlib>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "cpu/o3/vp/vp_key.hh"

namespace gem5
{
namespace o3
{

VtageTables::VtageTables(const VtageConfig &cfg, std::function<double()> rng)
    : baseEntries(cfg.baseEntries),
      taggedEntries(cfg.taggedEntries),
      numTagged(cfg.numTagged),
      historyLengths(cfg.historyLengths),
      baseTagBits(cfg.baseTagBits),
      confBits(cfg.confBits),
      confMax(cfg.confBits >= 32 ? ~0u : (1u << cfg.confBits) - 1),
      confThreshold(cfg.confThreshold),
      fpcVector(cfg.fpcVector),
      pathBits(cfg.pathBits),
      logBaseEntries(cfg.baseEntries > 0 ? floorLog2(cfg.baseEntries) : 0),
      logTaggedEntries(cfg.taggedEntries > 0 ? floorLog2(cfg.taggedEntries)
                                             : 0),
      indexFieldBits(std::max(logBaseEntries, logTaggedEntries)),
      tagFieldBits(cfg.baseTagBits + cfg.numTagged),
      rng(std::move(rng)),
      base(baseEntries),
      tagged(numTagged, std::vector<TaggedEntry>(taggedEntries))
{
    fatal_if(cfg.baseEntries == 0 || !isPowerOf2(cfg.baseEntries),
             "VTAGE baseEntries (%u) must be a nonzero power of two",
             cfg.baseEntries);
    fatal_if(cfg.taggedEntries == 0 || !isPowerOf2(cfg.taggedEntries),
             "VTAGE taggedEntries (%u) must be a nonzero power of two",
             cfg.taggedEntries);
    // The biased-rank token field packs 0 = none, 1 = VT0,
    // 2..(1+numTagged) = VT1..VTnumTagged (design doc, "Data
    // Structures").
    fatal_if(cfg.numTagged < 1 || cfg.numTagged > 7,
             "VTAGE numTagged (%u) must be in [1, 7]: the biased-rank "
             "token field packs 0 = none, 1 = VT0, 2..(1+numTagged) = "
             "tagged components",
             cfg.numTagged);
    fatal_if(cfg.historyLengths.size() != cfg.numTagged,
             "VTAGE historyLengths has %zu entries, expected numTagged "
             "(%u)",
             cfg.historyLengths.size(), cfg.numTagged);
    for (unsigned length : cfg.historyLengths) {
        fatal_if(length == 0 || length > 64,
                 "VTAGE history length (%u) must be in [1, 64]: ghr is "
                 "a 64-bit snapshot",
                 length);
    }
    fatal_if(cfg.confBits == 0 || cfg.confBits > 16,
             "VTAGE confBits (%u) must be in [1, 16]", cfg.confBits);
    // Threshold 0 makes conf >= confThreshold a tautology: the
    // verify-site corrective reset (c = 0) could then never stop a
    // refetched instance from being re-predicted -- the same
    // no-livelock guard LvpTable enforces on its own confThreshold.
    fatal_if(cfg.confThreshold < 1,
             "VTAGE confThreshold must be >= 1: 0 predicts on every "
             "hit, so the corrective reset could never leave an entry "
             "below threshold (squash-livelock hazard)");
    fatal_if(cfg.confThreshold > confMax,
             "VTAGE confThreshold (%u) exceeds the %u-bit counter max "
             "(%u)",
             cfg.confThreshold, cfg.confBits, confMax);
    fatal_if(cfg.fpcVector.size() != confMax,
             "VTAGE fpcVector has %zu entries, expected one per "
             "forward transition (confMax = %u)",
             cfg.fpcVector.size(), confMax);
    fatal_if(cfg.pathBits == 0 || cfg.pathBits > 16,
             "VTAGE pathBits (%u) must be in [1, 16]: path is a "
             "16-bit snapshot register",
             cfg.pathBits);
    fatal_if(cfg.baseTagBits < 1, "VTAGE baseTagBits (%u) must be >= 1",
             cfg.baseTagBits);
    // F()'s path-hash rotates by up to `numTagged` bits within a
    // logTaggedEntries-bit register (.superpowers/sdd/tage-folds.md
    // Sec. 4); the rotation amount must stay strictly smaller than
    // the register width.
    fatal_if(cfg.numTagged >= logTaggedEntries,
             "VTAGE numTagged (%u) must be < log2(taggedEntries) (%u)",
             cfg.numTagged, logTaggedEntries);
    fatal_if(rankFieldBits + indexFieldBits + tagFieldBits > 64,
             "VTAGE token geometry does not fit 64 bits: rank %u + "
             "index %u + tag %u",
             rankFieldBits, indexFieldBits, tagFieldBits);
}

uint64_t
VtageTables::shiftedPcOf(Addr pc, MicroPC upc) const
{
    // vp_key.hh's fold (pc XOR (upc << 48)) in place of TAGE's
    // instShiftAmt-only pre-shift; NOT truncated to 32 bits the way
    // gem5's TAGE truncates Addr -> unsigned int, since the upc lives
    // in bits [48:63] and truncating would discard the micro-PC
    // separation vp_key.hh exists to provide
    // (.superpowers/sdd/tage-folds.md Sec. 4, caveat 2).
    //
    // The trailing "XOR upc" is what actually DELIVERS micro-op
    // separation into the hashed tables: bits [48:63] never survive
    // the low, masked bits every index/tag computation below keeps
    // (confirmed by a 120k-lookup randomized sweep before this fix
    // found it arithmetically inert), so without this trailing XOR
    // two micro-ops of the same macro-op would alias identical
    // indices/tags.
    return (vpKey(pc, upc) >> 2) ^ upc;
}

uint64_t
VtageTables::foldHistory(uint64_t ghr, unsigned origLength,
                         unsigned compLength) const
{
    if (compLength == 0) {
        return 0;
    }
    uint64_t comp = 0;
    for (unsigned i = 0; i < origLength; i++) {
        // Bits replay oldest-first (chronological insertion order):
        // ghr bit 0 is newest, so the bit inserted at step i is ghr
        // bit (origLength - 1 - i).
        const uint64_t bit = (ghr >> (origLength - 1 - i)) & 1ULL;
        comp = (comp << 1) | bit;
        comp ^= (comp >> compLength);
        comp &= (1ULL << compLength) - 1;
    }
    return comp;
}

uint64_t
VtageTables::pathHash(uint64_t path, unsigned size, unsigned bank) const
{
    uint64_t a = path & ((1ULL << size) - 1);
    const uint64_t a1 = a & ((1ULL << logTaggedEntries) - 1);
    uint64_t a2 = a >> logTaggedEntries;
    a2 = ((a2 << bank) & ((1ULL << logTaggedEntries) - 1)) +
         (a2 >> (logTaggedEntries - bank));
    a = a1 ^ a2;
    a = ((a << bank) & ((1ULL << logTaggedEntries) - 1)) +
        (a >> (logTaggedEntries - bank));
    return a;
}

unsigned
VtageTables::computeIndex(uint64_t shiftedPc, const VpHistSnapshot &h,
                          unsigned bank) const
{
    const unsigned length = historyLengths[bank - 1];
    const unsigned hlen = std::min(length, pathBits);
    const uint64_t idxFold = foldHistory(h.ghr, length, logTaggedEntries);
    const uint64_t pathFold = pathHash(h.path, hlen, bank);
    const int shiftAmt =
        std::abs(static_cast<int>(logTaggedEntries) - static_cast<int>(bank)) +
        1;
    const uint64_t index =
        shiftedPc ^ (shiftedPc >> shiftAmt) ^ idxFold ^ pathFold;
    return static_cast<unsigned>(index & ((1ULL << logTaggedEntries) - 1));
}

uint64_t
VtageTables::computeTag(uint64_t shiftedPc, const VpHistSnapshot &h,
                        unsigned bank) const
{
    const unsigned length = historyLengths[bank - 1];
    const unsigned width = baseTagBits + bank;
    const uint64_t fold0 = foldHistory(h.ghr, length, width);
    const uint64_t fold1 = foldHistory(h.ghr, length, width - 1);
    const uint64_t tag = shiftedPc ^ fold0 ^ (fold1 << 1);
    return tag & ((1ULL << width) - 1);
}

unsigned
VtageTables::computeBaseIndex(uint64_t shiftedPc) const
{
    return static_cast<unsigned>(shiftedPc & (baseEntries - 1));
}

VtageTables::Provider
VtageTables::findProvider(Addr pc, MicroPC upc, const VpHistSnapshot &h) const
{
    const uint64_t shiftedPc = shiftedPcOf(pc, upc);
    // Longest-history-first scan; first tag hit is the provider
    // (.superpowers/sdd/tage-folds.md Sec. 10).
    for (unsigned bank = numTagged; bank > 0; bank--) {
        const unsigned index = computeIndex(shiftedPc, h, bank);
        const uint64_t tag = computeTag(shiftedPc, h, bank);
        const TaggedEntry &e = tagged[bank - 1][index];
        if (e.valid && e.tag == tag) {
            return Provider{bank, index, tag};
        }
    }
    return Provider{0, computeBaseIndex(shiftedPc), 0};
}

uint64_t
VtageTables::packToken(unsigned rank, unsigned index, uint64_t tag) const
{
    uint64_t token = static_cast<uint64_t>(rank);
    token |= static_cast<uint64_t>(index) << rankFieldBits;
    token |= tag << (rankFieldBits + indexFieldBits);
    return token;
}

VtageTables::UnpackedToken
VtageTables::unpackToken(uint64_t token) const
{
    UnpackedToken u;
    u.rank = token & ((1ULL << rankFieldBits) - 1);
    u.index = (token >> rankFieldBits) & ((1ULL << indexFieldBits) - 1);
    u.tag = (token >> (rankFieldBits + indexFieldBits)) &
            ((1ULL << tagFieldBits) - 1);
    return u;
}

VtageLookup
VtageTables::lookup(Addr pc, MicroPC upc, const VpHistSnapshot &h) const
{
    const Provider p = findProvider(pc, upc, h);
    VtageLookup result;
    // VT0 is tagless and always "hits": every lookup stamps a usable
    // token, whether or not it clears the confidence threshold
    // (design doc, "Predict").
    result.hit = true;
    if (p.bank == 0) {
        const BaseEntry &e = base[p.index];
        result.value = e.val;
        result.confident = e.c >= confThreshold;
        result.token = packToken(1, p.index, 0);
    } else {
        const TaggedEntry &e = tagged[p.bank - 1][p.index];
        result.value = e.val;
        result.confident = e.c >= confThreshold;
        result.token = packToken(p.bank + 1, p.index, p.tag);
    }
    return result;
}

bool
VtageTables::correctiveReset(uint64_t token)
{
    if (token == 0) {
        return false;
    }
    const UnpackedToken u = unpackToken(token);
    if (u.rank == 0) {
        return false;
    }
    if (u.rank == 1) {
        if (u.index >= baseEntries) {
            return false;
        }
        base[u.index].c = 0;
        return true;
    }
    const unsigned bank = u.rank - 1;
    if (bank > numTagged || u.index >= taggedEntries) {
        return false;
    }
    TaggedEntry &e = tagged[bank - 1][u.index];
    if (!e.valid || e.tag != u.tag) {
        return false; // Stale: entry reallocated in flight.
    }
    e.c = 0;
    e.u = false;
    return true;
}

std::vector<VtageTrainOutcome>
VtageTables::train(Addr pc, MicroPC upc, const VpHistSnapshot &h,
                   uint64_t token, RegVal actual)
{
    std::vector<VtageTrainOutcome> outcomes;

    const UnpackedToken u = unpackToken(token);
    bool stale = false;
    unsigned providerBank = 0;
    unsigned providerIndex = 0;

    if (token == 0 || u.rank == 0) {
        stale = true;
    } else if (u.rank == 1) {
        if (u.index >= baseEntries) {
            stale = true;
        } else {
            providerIndex = u.index;
        }
    } else {
        const unsigned bank = u.rank - 1;
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
        outcomes.push_back(VtageTrainOutcome::StaleTokenRecomputed);
        const Provider p = findProvider(pc, upc, h);
        providerBank = p.bank;
        providerIndex = p.index;
    }

    RegVal *valPtr;
    unsigned *cPtr;
    bool *uPtr = nullptr;
    if (providerBank == 0) {
        valPtr = &base[providerIndex].val;
        cPtr = &base[providerIndex].c;
    } else {
        TaggedEntry &e = tagged[providerBank - 1][providerIndex];
        valPtr = &e.val;
        cPtr = &e.c;
        uPtr = &e.u;
    }

    const bool correct = (*valPtr == actual);

    if (correct) {
        if (uPtr) {
            *uPtr = true;
        }
        // FPC: the i -> i+1 transition fires with probability
        // fpcVector[i]; already-saturated counters have no further
        // transition to attempt.
        if (*cPtr >= confMax) {
            outcomes.push_back(VtageTrainOutcome::CorrectSat);
        } else if (rng() < fpcVector[*cPtr]) {
            (*cPtr)++;
            outcomes.push_back(VtageTrainOutcome::CorrectInc);
        } else {
            outcomes.push_back(VtageTrainOutcome::CorrectSat);
        }
        return outcomes;
    }

    // Wrong: unconditional, never probabilistic.
    if (uPtr) {
        *uPtr = false;
    }
    if (*cPtr == 0) {
        *valPtr = actual;
        outcomes.push_back(VtageTrainOutcome::WrongValOverwrite);
    } else {
        *cPtr = 0;
        outcomes.push_back(VtageTrainOutcome::WrongReset);
    }

    // Allocation probe: components with rank above the provider
    // (providerBank == 0 for VT0 means every tagged component
    // qualifies as "above").
    const unsigned startBank = providerBank + 1;
    if (startBank <= numTagged) {
        const uint64_t shiftedPc = shiftedPcOf(pc, upc);
        std::vector<unsigned> candBank;
        std::vector<unsigned> candIndex;
        std::vector<uint64_t> candTag;
        std::vector<unsigned> qualifying;
        for (unsigned bank = startBank; bank <= numTagged; bank++) {
            const unsigned index = computeIndex(shiftedPc, h, bank);
            const uint64_t tag = computeTag(shiftedPc, h, bank);
            candBank.push_back(bank);
            candIndex.push_back(index);
            candTag.push_back(tag);
            if (!tagged[bank - 1][index].u) {
                qualifying.push_back(candBank.size() - 1);
            }
        }
        if (!qualifying.empty()) {
            unsigned pick = static_cast<unsigned>(rng() * qualifying.size());
            if (pick >= qualifying.size()) {
                pick = qualifying.size() - 1; // Defensive clamp.
            }
            const unsigned c = qualifying[pick];
            TaggedEntry &e = tagged[candBank[c] - 1][candIndex[c]];
            e.valid = true;
            e.val = actual;
            e.c = 0;
            e.u = false;
            e.tag = candTag[c];
            outcomes.push_back(VtageTrainOutcome::Allocated);
        } else {
            for (unsigned i = 0; i < candBank.size(); i++) {
                tagged[candBank[i] - 1][candIndex[i]].u = false;
            }
            outcomes.push_back(VtageTrainOutcome::AllocFailedAged);
        }
    }

    return outcomes;
}

} // namespace o3
} // namespace gem5
