/**
 * @file
 * Pythia, a data prefetcher that learns with reinforcement learning.
 * Reference:
 *   Bera, R., Kanellopoulos, K., Nori, A., Shahroodi, T., Subramoney, S.,
 *   Mutlu, O. (2021). Pythia: A Customizable Hardware Prefetching
 *   Framework Using Online Reinforcement Learning. MICRO-54.
 *
 * This follows the module the authors wrote for the 4th Data Prefetching
 * Championship (CMU-SAFARI/ChampSim, prefetcher/pythia), with the
 * configuration that module ships.
 *
 * On every access the prefetcher picks one action, a block delta inside
 * the page of the access (or no prefetch), with a SARSA agent. The agent
 * sees a state made of the PC and the last delta on the page, and scores
 * each action with two tile-coded Q-tables, one per feature (the PC alone
 * and the PC with the delta), keeping the larger score. Each action is
 * recorded in a FIFO tracker. A later demand for the prefetched block
 * rewards it, more if the block had already been filled. An action that
 * leaves the tracker unrewarded is penalised, and the agent is trained
 * with each pair of consecutive actions that leave it.
 *
 * As in the ChampSim module, which acts on loads and prefetches only,
 * reads for ownership and page-table walks are ignored, and prefetches
 * from an upper level are observed with a PC of 0 whether or not they
 * were tagged with one.
 *
 * Differences from the ChampSim module:
 *  - The memory-bandwidth signal is absent: the agent always behaves as
 *    under low bandwidth usage, so the separate high-bandwidth rewards
 *    are not modelled.
 *  - Only the two features of the shipped configuration exist.
 *  - Other accesses with no PC reward earlier prefetches but neither
 *    train nor trigger one.
 */

#ifndef __MEM_CACHE_PREFETCH_PYTHIA_HH__
#define __MEM_CACHE_PREFETCH_PYTHIA_HH__

#include <deque>
#include <list>
#include <optional>
#include <vector>

#include "base/random.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{

struct PythiaPrefetcherParams;

namespace prefetch
{

class Pythia : public Queued
{
  private:
    /** What the agent observes at an access. */
    struct State
    {
        Addr pc;
        /** Block delta from the previous access to the same page. */
        int delta;
    };

    /** The inputs of the Q-tables. */
    enum Feature
    {
        FeaturePC,
        FeaturePCDelta,
        NumFeatures
    };

    /** The outcomes an action is rewarded for. */
    enum Outcome
    {
        NoPrefetch,
        Incorrect,
        CorrectUntimely,
        CorrectTimely,
        OutOfPage,
        NumOutcomes
    };

    /** How often a delta was newly prefetched on a page. */
    struct DeltaConfidence
    {
        int delta;
        int confidence;
    };

    /** Per-page history. */
    struct PageEntry
    {
        Addr page;
        unsigned lastOffset;
        int lastDelta;
        /** Blocks of the page already predicted. */
        std::vector<bool> predicted;
        /** The most recently prefetched deltas, oldest first. */
        std::deque<DeltaConfidence> recentDeltas;
    };

    /** An action waiting for its reward. */
    struct TrackedAction
    {
        /** The prefetched block, MaxAddr if nothing was prefetched. */
        Addr addr;
        State state;
        unsigned action;
        bool filled = false;
        bool rewarded = false;
        int reward = 0;
    };

    /** Learning rate, discount factor and exploration rate. */
    const float alpha;
    const float gamma;
    const float epsilon;

    /** The block deltas the agent chooses from; 0 means no prefetch. */
    const std::vector<int> actions;

    const unsigned pageTableSize;
    const unsigned trackerSize;
    const unsigned recentDeltasSize;

    /** Reward of each outcome. */
    const std::vector<int> rewards;

    /**
     * Degree selection: degrees[i] is used while the confidence of the
     * chosen delta on the page is at most degreeThresholds[i], and the
     * last degree beyond the last threshold.
     */
    const std::vector<int> degreeThresholds;
    const std::vector<unsigned> degrees;

    const unsigned numTilings;
    const unsigned numTiles;

    const unsigned blocksPerPage;

    /** Pages in LRU order, the most recently used last. */
    std::list<PageEntry> pageTable;

    /** Actions in the order they were taken. */
    std::deque<TrackedAction> tracker;

    /** The action that left the tracker last, kept to train with. */
    std::optional<TrackedAction> lastEvicted;

    /** Q-values, indexed by feature, tiling, tile and action. */
    std::vector<float> qTable;

    Random::RandomPtr rng;

    /** Whether the access being observed is an upper-level prefetch. */
    bool observingPrefetch = false;

    struct PythiaStats : public statistics::Group
    {
        PythiaStats(Pythia &parent);

        statistics::Vector actionsChosen;
        statistics::Scalar actionsExplored;
        statistics::Scalar alreadyTracked;
        statistics::Scalar extraDegreePrefetches;
        statistics::Vector outcomes;
        statistics::Scalar trainings;
        statistics::Scalar pageTableMisses;
        statistics::Scalar accessesWithoutPC;
        statistics::Scalar ignoredAccesses;
        statistics::Scalar upperLevelPrefetches;
    } stats;

    float &qValue(unsigned feature, unsigned tiling, unsigned tile,
                  unsigned action);

    /** The tile a state falls on in one tiling of a feature. */
    unsigned tileIndex(unsigned feature, unsigned tiling,
                       const State &state) const;

    /** Q-value of an action for one feature: the sum over its tilings. */
    float featureQ(unsigned feature, const State &state, unsigned action);

    /** Q-value of an action: the largest over the features. */
    float actionQ(const State &state, unsigned action);

    unsigned chooseAction(const State &state);

    /** SARSA update of every Q-table. */
    void learn(const State &state, unsigned action, int reward,
               const State &next_state, unsigned next_action);

    /** Finds the page's history, or starts it; updates it with offset. */
    PageEntry &updatePage(Addr page, unsigned offset);

    /** Prefetch degree for a delta, from its confidence on the page. */
    unsigned prefetchDegree(const PageEntry &entry, int delta) const;

    /** Records that a delta predicted a block of the page. */
    void recordPrediction(PageEntry &entry, unsigned offset, int delta);

    /** The oldest tracked action that prefetched addr, if any. */
    TrackedAction *findTracked(Addr addr);

    /**
     * Adds an action to the tracker, training with the one it pushes
     * out.
     * @return the new entry, or nullptr if addr is already tracked
     */
    TrackedAction *track(Addr addr, const State &state, unsigned action);

    void assignReward(TrackedAction &entry, Outcome outcome);

    /** Rewards the tracked prefetch of a block that is now demanded. */
    void rewardDemand(Addr addr);

  public:
    Pythia(const PythiaPrefetcherParams &p);
    ~Pythia() = default;

    /** Ignores reads for ownership and page-table walks. */
    void notify(const CacheAccessProbeArg &acc,
                const PrefetchInfo &pfi) override;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses,
                           const CacheAccessor &cache) override;

    /** Marks the tracked prefetch of a filled block as timely. */
    void notifyFill(const CacheAccessProbeArg &arg) override;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_PYTHIA_HH__
