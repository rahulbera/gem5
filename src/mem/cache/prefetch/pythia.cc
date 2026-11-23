#include "mem/cache/prefetch/pythia.hh"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "base/cprintf.hh"
#include "base/logging.hh"
#include "debug/HWPrefetch.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/PythiaPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

namespace
{

/** One constant per tiling, so that the tilings of a feature differ. */
const uint32_t tilingOffsets[] = {
    0xaca081b9, 0x666a1c67, 0xc11d6a53, 0x8e5d97c1, 0x0d1cad54, 0x874f71cb,
    0x20d2fa13, 0x73f7c4a7, 0x0b701f6c, 0x8388d86d, 0xf72ac9f2, 0xbab16d82,
    0x524ac258, 0xb5900302, 0xb48ccc72, 0x632f05bf, 0xe7111073, 0xeb602af4,
    0xf3f29ebb, 0x2a6184f2, 0x461da5da, 0x6693471d, 0x62fd0138, 0xc484efb3,
    0x81c9eeeb, 0x860f3766, 0x334faf86, 0x5e81e881, 0x14bc2195, 0xf47671a8,
    0x75414279, 0x357bc5e0};

/** Bits a delta takes in the PC-and-delta feature, sign included. */
const unsigned deltaBits = 7;

/** Robert Jenkins' 32-bit mix function. */
uint32_t
jenkinsHash(uint32_t key)
{
    key += (key << 12);
    key ^= (key >> 22);
    key += (key << 4);
    key ^= (key >> 9);
    key += (key << 10);
    key ^= (key >> 2);
    key += (key << 7);
    key ^= (key >> 12);
    return key;
}

const char *outcomeNames[] = {"noPrefetch", "incorrect", "correctUntimely",
                              "correctTimely", "outOfPage"};

} // anonymous namespace

Pythia::Pythia(const PythiaPrefetcherParams &p)
    : Queued(p),
      alpha(p.alpha),
      gamma(p.gamma),
      epsilon(p.epsilon),
      actions(p.actions),
      pageTableSize(p.page_table_size),
      trackerSize(p.tracker_size),
      recentDeltasSize(p.recent_deltas_size),
      rewards{p.reward_no_prefetch, p.reward_incorrect,
              p.reward_correct_untimely, p.reward_correct_timely,
              p.reward_out_of_page},
      degreeThresholds(p.degree_thresholds),
      degrees(p.degrees),
      numTilings(p.num_tilings),
      numTiles(p.num_tiles),
      blocksPerPage(pageBytes / blkSize),
      rng(Random::genRandom(p.seed)),
      stats(*this)
{
    fatal_if(actions.empty(), "%s: no actions", name());
    fatal_if(degrees.size() != degreeThresholds.size() + 1,
             "%s: degrees must have one entry more than degree_thresholds",
             name());
    fatal_if(numTilings == 0 || numTilings > std::size(tilingOffsets),
             "%s: num_tilings must be between 1 and %d", name(),
             std::size(tilingOffsets));
    fatal_if(numTiles == 0 || trackerSize == 0 || pageTableSize == 0,
             "%s: table sizes must not be zero", name());
    fatal_if(gamma >= 1, "%s: gamma must be below 1", name());

    // Optimistic start: every action looks as good as an endless run of
    // unit rewards, so each gets tried before the agent settles.
    qTable.assign(NumFeatures * numTilings * numTiles * actions.size(),
                  1.0f / (1.0f - gamma));
}

float &
Pythia::qValue(unsigned feature, unsigned tiling, unsigned tile,
               unsigned action)
{
    return qTable[((feature * numTilings + tiling) * numTiles + tile) *
                      actions.size() +
                  action];
}

unsigned
Pythia::tileIndex(unsigned feature, unsigned tiling, const State &state) const
{
    uint64_t key = state.pc;
    if (feature == FeaturePCDelta) {
        // Sign and magnitude, so that a delta and its opposite differ.
        const uint64_t delta = state.delta < 0
                                   ? -state.delta + (1 << (deltaBits - 1))
                                   : state.delta;
        key = (key << deltaBits) + delta;
    }
    const uint32_t folded =
        static_cast<uint32_t>(key) ^ static_cast<uint32_t>(key >> 32);
    return jenkinsHash(folded ^ tilingOffsets[tiling]) % numTiles;
}

float
Pythia::featureQ(unsigned feature, const State &state, unsigned action)
{
    float q = 0;
    for (unsigned tiling = 0; tiling < numTilings; tiling++) {
        q +=
            qValue(feature, tiling, tileIndex(feature, tiling, state), action);
    }
    return q;
}

float
Pythia::actionQ(const State &state, unsigned action)
{
    float q = featureQ(0, state, action);
    for (unsigned feature = 1; feature < NumFeatures; feature++) {
        q = std::max(q, featureQ(feature, state, action));
    }
    return q;
}

unsigned
Pythia::chooseAction(const State &state)
{
    unsigned action = 0;
    // An integer draw: Random's floating-point one is not uniform.
    const uint64_t draw = rng->random<uint64_t>(0, UINT32_MAX);
    if (draw < epsilon * 4294967296.0) {
        action = rng->random<unsigned>(0, actions.size() - 1);
        stats.actionsExplored++;
    } else {
        // The first action stands unless another has a positive Q-value
        // above it, and when no Q-value is positive.
        float best = 0;
        for (unsigned a = 0; a < actions.size(); a++) {
            const float q = actionQ(state, a);
            if (q > best) {
                best = q;
                action = a;
            }
        }
    }
    stats.actionsChosen[action]++;
    return action;
}

void
Pythia::learn(const State &state, unsigned action, int reward,
              const State &next_state, unsigned next_action)
{
    for (unsigned feature = 0; feature < NumFeatures; feature++) {
        for (unsigned tiling = 0; tiling < numTilings; tiling++) {
            const float next_q =
                qValue(feature, tiling, tileIndex(feature, tiling, next_state),
                       next_action);
            float &q = qValue(feature, tiling,
                              tileIndex(feature, tiling, state), action);
            q += alpha * (reward + gamma * next_q - q);
        }
    }
    stats.trainings++;
}

Pythia::PageEntry &
Pythia::updatePage(Addr page, unsigned offset)
{
    auto it = std::find_if(
        pageTable.begin(), pageTable.end(),
        [page](const PageEntry &entry) { return entry.page == page; });
    if (it != pageTable.end()) {
        it->lastDelta =
            static_cast<int>(offset) - static_cast<int>(it->lastOffset);
        it->lastOffset = offset;
        pageTable.splice(pageTable.end(), pageTable, it);
    } else {
        stats.pageTableMisses++;
        if (pageTable.size() >= pageTableSize) {
            pageTable.pop_front();
        }
        pageTable.push_back(
            {page, offset, 0, std::vector<bool>(blocksPerPage, false), {}});
    }
    return pageTable.back();
}

unsigned
Pythia::prefetchDegree(const PageEntry &entry, int delta) const
{
    auto it = std::find_if(
        entry.recentDeltas.begin(), entry.recentDeltas.end(),
        [delta](const DeltaConfidence &dc) { return dc.delta == delta; });
    if (it == entry.recentDeltas.end()) {
        return 1;
    }

    for (unsigned i = 0; i < degreeThresholds.size(); i++) {
        if (it->confidence <= degreeThresholds[i]) {
            return degrees[i];
        }
    }
    return degrees.back();
}

void
Pythia::recordPrediction(PageEntry &entry, unsigned offset, int delta)
{
    if (entry.predicted[offset]) {
        return;
    }
    entry.predicted[offset] = true;

    auto it = std::find_if(
        entry.recentDeltas.begin(), entry.recentDeltas.end(),
        [delta](const DeltaConfidence &dc) { return dc.delta == delta; });
    if (it != entry.recentDeltas.end()) {
        DeltaConfidence dc = *it;
        dc.confidence++;
        entry.recentDeltas.erase(it);
        entry.recentDeltas.push_back(dc);
    } else {
        if (entry.recentDeltas.size() >= recentDeltasSize) {
            entry.recentDeltas.pop_front();
        }
        entry.recentDeltas.push_back({delta, 0});
    }
}

Pythia::TrackedAction *
Pythia::findTracked(Addr addr)
{
    auto it = std::find_if(
        tracker.begin(), tracker.end(),
        [addr](const TrackedAction &entry) { return entry.addr == addr; });
    return it == tracker.end() ? nullptr : &*it;
}

Pythia::TrackedAction *
Pythia::track(Addr addr, const State &state, unsigned action)
{
    if (addr != MaxAddr && findTracked(addr)) {
        stats.alreadyTracked++;
        return nullptr;
    }

    if (tracker.size() >= trackerSize) {
        const TrackedAction evicted = tracker.front();
        tracker.pop_front();
        if (lastEvicted) {
            // Nothing demanded what the earlier action prefetched.
            if (!lastEvicted->rewarded) {
                assignReward(*lastEvicted, lastEvicted->addr == MaxAddr
                                               ? NoPrefetch
                                               : Incorrect);
            }
            learn(lastEvicted->state, lastEvicted->action, lastEvicted->reward,
                  evicted.state, evicted.action);
        }
        lastEvicted = evicted;
    }

    tracker.push_back({addr, state, action});
    return &tracker.back();
}

void
Pythia::assignReward(TrackedAction &entry, Outcome outcome)
{
    assert(!entry.rewarded);
    entry.reward = rewards[outcome];
    entry.rewarded = true;
    stats.outcomes[outcome]++;
}

void
Pythia::rewardDemand(Addr addr)
{
    TrackedAction *entry = findTracked(addr);
    if (entry && !entry->rewarded) {
        assignReward(*entry, entry->filled ? CorrectTimely : CorrectUntimely);
    }
}

void
Pythia::notify(const CacheAccessProbeArg &acc, const PrefetchInfo &pfi)
{
    const RequestPtr &req = acc.pkt->req;
    // Requests made by a prefetcher of an upper level.
    const bool prefetch = req->taskId() == context_switch_task_id::Prefetcher;

    // The ChampSim module sees only loads and prefetches: stores (reads
    // for ownership) and translations neither reward nor train.
    if (!prefetch && (acc.pkt->needsWritable() || req->isPTWalk())) {
        stats.ignoredAccesses++;
        return;
    }

    observingPrefetch = prefetch;
    Queued::notify(acc, pfi);
    observingPrefetch = false;
}

void
Pythia::calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses,
                          const CacheAccessor &cache)
{
    const Addr addr = blockAddress(pfi.getAddr());
    rewardDemand(addr);

    // ChampSim prefetch requests carry no PC, so the module sees upper
    // level prefetches with a PC of 0; do the same, tagged or not.
    Addr pc = 0;
    if (observingPrefetch) {
        stats.upperLevelPrefetches++;
    } else if (pfi.hasPC()) {
        pc = pfi.getPC();
    } else {
        stats.accessesWithoutPC++;
        return;
    }

    const Addr page = addr / pageBytes;
    const Addr page_addr = page * pageBytes;
    const int offset = (addr - page_addr) >> lBlkSize;

    PageEntry &entry = updatePage(page, offset);
    const State state{pc, entry.lastDelta};

    const unsigned action = chooseAction(state);
    const int delta = actions[action];
    const unsigned degree = prefetchDegree(entry, delta);

    if (delta == 0) {
        track(MaxAddr, state, action);
        return;
    }

    const int target = offset + delta;
    if (target < 0 || target >= static_cast<int>(blocksPerPage)) {
        assignReward(*track(MaxAddr, state, action), OutOfPage);
        return;
    }

    const Addr pf_addr = page_addr + (static_cast<Addr>(target) << lBlkSize);
    if (!track(pf_addr, state, action)) {
        return;
    }

    DPRINTF(HWPrefetch, "Pythia: pc %#x addr %#x delta %d degree %d\n",
            state.pc, addr, delta, degree);
    addresses.push_back(AddrPriority(pf_addr, 0));
    recordPrediction(entry, target, delta);

    // Only the first block is tracked; the others ride on its reward.
    for (int d = 2; d <= static_cast<int>(degree); d++) {
        const int next = offset + d * delta;
        if (next < 0 || next >= static_cast<int>(blocksPerPage)) {
            continue;
        }
        addresses.push_back(AddrPriority(
            page_addr + (static_cast<Addr>(next) << lBlkSize), 0));
        stats.extraDegreePrefetches++;
    }
}

void
Pythia::notifyFill(const CacheAccessProbeArg &arg)
{
    TrackedAction *entry = findTracked(blockAddress(arg.pkt->getAddr()));
    if (entry) {
        entry->filled = true;
    }
}

Pythia::PythiaStats::PythiaStats(Pythia &parent)
    : statistics::Group(&parent),
      ADD_STAT(actionsChosen, statistics::units::Count::get(),
               "times each action was chosen, by its block delta "
               "(p: forward, n: backward, none: no prefetch)"),
      ADD_STAT(actionsExplored, statistics::units::Count::get(),
               "actions chosen at random to explore"),
      ADD_STAT(alreadyTracked, statistics::units::Count::get(),
               "actions dropped because their block was already tracked"),
      ADD_STAT(extraDegreePrefetches, statistics::units::Count::get(),
               "prefetch candidates beyond the first of an action"),
      ADD_STAT(outcomes, statistics::units::Count::get(),
               "rewarded actions by outcome"),
      ADD_STAT(trainings, statistics::units::Count::get(), "Q-table updates"),
      ADD_STAT(pageTableMisses, statistics::units::Count::get(),
               "accesses to a page with no history"),
      ADD_STAT(accessesWithoutPC, statistics::units::Count::get(),
               "accesses not acted on because they carry no PC"),
      ADD_STAT(ignoredAccesses, statistics::units::Count::get(),
               "reads for ownership and page-table walks ignored"),
      ADD_STAT(upperLevelPrefetches, statistics::units::Count::get(),
               "upper-level prefetches observed with a PC of 0")
{
    actionsChosen.init(parent.actions.size());
    for (unsigned a = 0; a < parent.actions.size(); a++) {
        const int delta = parent.actions[a];
        actionsChosen.subname(
            a, delta == 0
                   ? "none"
                   : csprintf("%s%d", delta > 0 ? "p" : "n", std::abs(delta)));
    }

    outcomes.init(NumOutcomes);
    for (unsigned o = 0; o < NumOutcomes; o++) {
        outcomes.subname(o, outcomeNames[o]);
    }
}

} // namespace prefetch
} // namespace gem5
