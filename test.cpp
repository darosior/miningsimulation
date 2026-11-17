#include <algorithm>
#include <cmath>
#include <iostream>

#include "simulation.h"

/**
 * Some of the tests below are statistical: they measure a random quantity many times and compare the result to
 * the value we know it must average to. The two never match exactly, so we accept a measure which is within 5
 * standard deviations of the expected value: a correct implementation fails this about once in a million times.
 * These tests use fixed seeds, so they give the same result every time they are run.
 */
static constexpr int MAX_STD_DEVS{5};

/** Whether seeing an event `count` times out of `trials` is consistent with it having a probability `p` to happen
 * each time. The count follows a binomial distribution: its mean is `trials * p` and its variance
 * `trials * p * (1 - p)`. */
bool MatchesProbability(long count, long trials, double p)
{
    const double expected{trials * p};
    const double std_dev{std::sqrt(trials * p * (1 - p))};
    return std::abs(count - expected) < MAX_STD_DEVS * std_dev;
}

/** A number of measures of the same random quantity, used to estimate its average. */
struct Sample {
    long count{0};
    double sum{0.0}, sum_squares{0.0};

    void Add(double value) {
        count++;
        sum += value;
        sum_squares += value * value;
    }

    double Mean() const { return sum / count; }

    /** The standard deviation of the mean: how far it can be expected to be from the true average. */
    double StdError() const {
        const double variance{sum_squares / count - Mean() * Mean()};
        return std::sqrt(variance / count);
    }

    /** Whether the mean of the measures is consistent with the given average for the quantity measured. */
    bool Matches(double expected) const {
        return std::abs(Mean() - expected) < MAX_STD_DEVS * StdError();
    }
};

/** The share of the network hashrate controlled by a miner, between 0 and 1. */
double Hashrate(const Miner& miner)
{
    return miner.perc / 100.0;
}

/** Each miner must find a share of the blocks equal to its share of the network hashrate. */
void TestMinerPicker()
{
    constexpr long BLOCK_COUNT{10'000'000};

    // Both large and small miners.
    std::vector<Miner> miners;
    miners.emplace_back(0, 35, 0s);
    miners.emplace_back(1, 20, 0s);
    miners.emplace_back(2, 18, 0s);
    miners.emplace_back(3, 15, 0s);
    miners.emplace_back(4, 11, 0s);
    miners.emplace_back(5, 1, 0s);

    // Count how many of the blocks each miner finds.
    RNG rng{0};
    std::vector<long> blocks_found(miners.size());
    for (long i{0}; i < BLOCK_COUNT; ++i) {
        blocks_found[PickFinder(miners, rng).id]++;
    }

    for (const auto& miner: miners) {
        assert(MatchesProbability(blocks_found[miner.id], BLOCK_COUNT, Hashrate(miner)));
    }

    std::cout << "Miner picker tests passed." << std::endl;
}

/** The time between two blocks must follow an exponential distribution with a mean of BLOCK_INTERVAL. */
void TestBlockInterval()
{
    constexpr long INTERVAL_COUNT{10'000'000};

    RNG rng{0};
    std::vector<std::chrono::milliseconds> intervals;
    for (long i{0}; i < INTERVAL_COUNT; ++i) {
        intervals.push_back(NextBlockInterval(rng));
    }

    // Their mean must be 10 minutes.
    Sample sample;
    for (const auto interval: intervals) {
        sample.Add(interval.count());
    }
    assert(sample.Matches(std::chrono::milliseconds{BLOCK_INTERVAL}.count()));

    // Other distributions have the same mean. What makes this one exponential is that the probability for an
    // interval to be shorter than a given duration is `1 - exp(-duration / mean)`. Check it for a few durations.
    for (const std::chrono::seconds duration: {10s, 60s, 600s, 3600s}) {
        const long shorter_count{std::ranges::count_if(intervals, [&](auto interval) { return interval < duration; })};
        const double p_shorter{1 - std::exp(-std::chrono::duration<double>(duration) / BLOCK_INTERVAL)};
        assert(MatchesProbability(shorter_count, INTERVAL_COUNT, p_shorter));
    }

    std::cout << "Block interval tests passed." << std::endl;
}

/**
 * The tests below run whole simulations and compare their outcome to what it is known to be in theory.
 */

//! How long each simulation is run for in these tests.
static constexpr std::chrono::days SIM_DURATION{365};

/** What we measure for a miner in each simulation. */
struct MinerSamples {
    Sample blocks_share, stale_rate;
};

/** Run 100 simulations with the given miners and record the outcome of each of them, for each miner. */
std::vector<MinerSamples> RunSimulations(const std::vector<Miner>& miners)
{
    std::vector<MinerSamples> samples(miners.size());
    for (uint64_t seed{0}; seed < 100; ++seed) {
        const auto stats{RunSimulation(SIM_DURATION, miners, seed)};
        for (size_t i{0}; i < miners.size(); ++i) {
            samples[i].blocks_share.Add(stats[i].blocks_share);
            samples[i].stale_rate.Add(stats[i].stale_rate);
        }
    }
    return samples;
}

/** When blocks propagate instantly, each miner must get a share of the blocks equal to its share of the network
 * hashrate and no block may go stale. */
void TestInstantPropagation()
{
    std::vector<Miner> miners;
    miners.emplace_back(0, 50, 0s);
    miners.emplace_back(1, 30, 0s);
    miners.emplace_back(2, 20, 0s);

    const auto results{RunSimulations(miners)};
    for (size_t i{0}; i < miners.size(); ++i) {
        assert(results[i].blocks_share.Matches(Hashrate(miners[i])));
        // The only blocks which may go stale are those found the very same millisecond as another one. This is
        // fewer than one block in 100'000.
        assert(results[i].stale_rate.Mean() < 1.0 / 100'000);
    }

    std::cout << "Instant propagation tests passed." << std::endl;
}

/** The probability for a block found by `miner` to go stale, when a block takes `propagation` to reach the miners
 * which did not find it. This is the model plotted by plot_stale_rate/plot.py.
 *
 * A block goes stale when another miner finds a competing block (the two miners had not received each other's),
 * and the next block is found on top of this other block. This ignores what happens when more than two blocks
 * compete, which makes it accurate to within about `propagation / BLOCK_INTERVAL`.
 */
double ExpectedStaleRate(const std::vector<Miner>& miners, const Miner& miner, std::chrono::milliseconds propagation)
{
    // The probability that miners with this share of the network hashrate find a block within `propagation`.
    const auto p_block_within_propagation = [&](double hashrate) {
        return 1 - std::exp(-hashrate * propagation / BLOCK_INTERVAL);
    };

    // The competing block may have been found right before ours. All the other miners received it before ours,
    // they mine on top of it. Our block goes stale unless we are the one finding the next block.
    const double others_hashrate{1 - Hashrate(miner)};
    double stale_rate{p_block_within_propagation(others_hashrate) * others_hashrate};

    // Or the competing block may be found right after ours. All the other miners received ours first, only its
    // finder mines on top of it. Our block goes stale if this miner also finds the next block.
    for (const auto& other: miners) {
        if (other.id == miner.id) continue;
        stale_rate += p_block_within_propagation(Hashrate(other)) * Hashrate(other);
    }

    return stale_rate;
}

/** When blocks take time to propagate, they must go stale at the rate given by ExpectedStaleRate(). This rate is
 * lower for larger miners, which must therefore get a larger share of the blocks than their share of hashrate. */
void TestPropagationDelay()
{
    constexpr std::chrono::seconds PROPAGATION{10};

    std::vector<Miner> miners;
    miners.emplace_back(0, 50, PROPAGATION);
    miners.emplace_back(1, 30, PROPAGATION);
    miners.emplace_back(2, 20, PROPAGATION);

    // The proportion of all the blocks found on the network which are expected to not go stale.
    double expected_not_stale{0.0};
    for (const auto& miner: miners) {
        expected_not_stale += Hashrate(miner) * (1 - ExpectedStaleRate(miners, miner, PROPAGATION));
    }

    const auto results{RunSimulations(miners)};
    for (size_t i{0}; i < miners.size(); ++i) {
        // The model is an approximation, see ExpectedStaleRate(). We accept a 5% difference with it.
        const double expected_stale_rate{ExpectedStaleRate(miners, miners[i], PROPAGATION)};
        assert(std::abs(results[i].stale_rate.Mean() / expected_stale_rate - 1) < 0.05);

        // The blocks in the best chain are those which did not go stale. The share of a miner in the best chain
        // is therefore the proportion of all blocks which are its own and not stale, over the proportion of all
        // blocks which are not stale.
        const double expected_share{Hashrate(miners[i]) * (1 - expected_stale_rate) / expected_not_stale};
        assert(results[i].blocks_share.Matches(expected_share));
    }

    std::cout << "Propagation delay tests passed." << std::endl;
}

/** The share of blocks expected for a selfish miner with a share `alpha` of the network hashrate. This is the
 * revenue of the pool given by equation (8) in the 2013 paper (https://arxiv.org/pdf/1311.0243) for gamma=0, i.e.
 * when no other miner ever mines on top of the selfish miner's block in a race. */
double ExpectedSelfishShare(double alpha)
{
    return (alpha * std::pow(1 - alpha, 2) * 4 * alpha - std::pow(alpha, 3)) / (1 - alpha * (1 + (2 - alpha) * alpha));
}

/** A selfish miner must get the share of blocks computed in the 2013 paper. */
void TestSelfishMining()
{
    // With less than a third of the network hashrate selfish mining is a loss (30% of the hashrate gets 27.3% of
    // the blocks), with more it is a gain (40% of the hashrate gets 48.4% of the blocks).
    for (const int selfish_perc: {30, 40}) {
        // As in the paper, blocks propagate instantly.
        std::vector<Miner> miners;
        miners.emplace_back(0, selfish_perc, 0s, Strategy::SELFISH);
        miners.emplace_back(1, 70 - selfish_perc, 0s);
        miners.emplace_back(2, 20, 0s);
        miners.emplace_back(3, 10, 0s);

        const auto results{RunSimulations(miners)};
        const double expected_selfish_share{ExpectedSelfishShare(Hashrate(miners[0]))};
        assert(results[0].blocks_share.Matches(expected_selfish_share));

        // The other miners split the remaining blocks in proportion to their hashrate.
        for (size_t i{1}; i < miners.size(); ++i) {
            const double expected_share{(1 - expected_selfish_share) * Hashrate(miners[i]) / (1 - Hashrate(miners[0]))};
            assert(results[i].blocks_share.Matches(expected_share));
        }
    }

    std::cout << "Selfish mining tests passed." << std::endl;
}

/** Add a chain of blocks on top of the given block, one for each of the given miners. Returns its last block. */
BlockId AddBlocks(BlockTree& blocks, BlockId parent, std::initializer_list<unsigned> miner_ids)
{
    for (const auto miner_id: miner_ids) {
        parent = blocks.Add(miner_id, parent);
    }
    return parent;
}

/** A selfish miner whose chain is the published chain ending in `public_tip`, followed by a number of blocks
 * it found on top of it and did not publish. */
Miner SelfishMiner(BlockTree& blocks, unsigned id, BlockId public_tip, size_t withheld_blocks)
{
    Miner selfish_miner{id, 35, 100ms, Strategy::SELFISH};
    selfish_miner.ReceivedBlock(blocks, public_tip);
    for (size_t i{0}; i < withheld_blocks; ++i) {
        selfish_miner.FoundBlock(blocks);
    }
    assert(selfish_miner.withheld_blocks == withheld_blocks);
    return selfish_miner;
}

/** Test our implementation of the "worst case" (gamma=0) selfish mining strategy. This goes over all the possible state in
 * model presented in section 4.2 of the 2013 paper. We also exercise some scenarii not present in the 2013 paper's model.
 */
void TestSelfishStrategy()
{
    constexpr int SM_ID{0}, OTHERS_ID{1};
    BlockTree blocks;

    /** Case (a), any state but two branches of length 1, pool finds a block. */
    {
        // Start with a public chain of 2 blocks (+ genesis)
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 0)};

        // Private fork of 0 block, best chain fork of 0 block, pool finds a block. "The pool appends
        // one block to its private branch, increasing its lead on the public branch by one."
        assert(!selfish_miner.FoundBlock(blocks)); // Not published.
        assert(blocks.Height(selfish_miner.tip) == 3);
        assert(selfish_miner.withheld_blocks == 1);

        // Private chain of 1 block, best chain fork of 0 block, pool finds a block. "The pool appends
        // one block to its private branch, increasing its lead on the public branch by one."
        assert(!selfish_miner.FoundBlock(blocks));
        assert(blocks.Height(selfish_miner.tip) == 4);
        assert(selfish_miner.withheld_blocks == 2);
        // Its chain is the public chain followed by its two blocks.
        assert(blocks.MinerId(selfish_miner.tip) == SM_ID);
        assert(blocks.MinerId(blocks.Parent(selfish_miner.tip)) == SM_ID);
        assert(blocks.Ancestor(selfish_miner.tip, 2) == public_tip);
    }

    /** Case (b), was two branches of length 1, pool finds a block. */
    {
        // A selfish miner with a 1-block private fork on top of a 3 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID, OTHERS_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 1)};
        const BlockId selfish_block{selfish_miner.tip};

        // The others find a block too. The selfish miner reveals his (see case (f) below) and there are now two
        // branches of length 1.
        const BlockId others_block{blocks.Add(OTHERS_ID, public_tip)};
        assert(selfish_miner.ReceivedBlock(blocks, others_block) == selfish_block);

        // Now the selfish miner finds a block. "The pool publishes its secret branch of length two".
        const auto published{selfish_miner.FoundBlock(blocks)};
        assert(published == selfish_miner.tip);
        assert(blocks.Parent(selfish_miner.tip) == selfish_block);
        assert(selfish_miner.withheld_blocks == 0);

        // The others receive it, and so does he. He won the race, and is back to withholding the blocks he finds.
        assert(!selfish_miner.ReceivedBlock(blocks, *published));
        assert(!selfish_miner.FoundBlock(blocks));
        assert(selfish_miner.withheld_blocks == 1);
    }

    /** Case (c), was two branches of length 1, others find a block after pool head. */
    // This never happens in our simulation since we only implement gamma=0

    /** Case (d), was two branches of length 1, others find a block after others’ head. */
    {
        // A selfish miner with a 1-block private fork on top of a 3 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID, OTHERS_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 1)};

        // The others find a block too. The selfish miner reveals his and there are now two branches of length 1.
        const BlockId others_block{blocks.Add(OTHERS_ID, public_tip)};
        assert(selfish_miner.ReceivedBlock(blocks, others_block) == selfish_miner.tip);

        // Now the selfish miner is notified of a longer best chain with the last two blocks being the others'. He
        // switches to mining on top of it.
        const BlockId others_next_block{blocks.Add(OTHERS_ID, others_block)};
        assert(!selfish_miner.ReceivedBlock(blocks, others_next_block));
        assert(selfish_miner.tip == others_next_block);

        // He lost the race, and is back to withholding the blocks he finds.
        assert(!selfish_miner.FoundBlock(blocks));
        assert(blocks.Parent(selfish_miner.tip) == others_next_block);
        assert(selfish_miner.withheld_blocks == 1);
    }

    /** Case (e), no private branch, others find a block. */
    {
        // A selfish miner with no private fork on top of a 4 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID, OTHERS_ID, SM_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 0)};

        // Now the selfish miner is notified of a longer best chain with the last block being the other's. He
        // switches to mining on top of it.
        const BlockId others_block{blocks.Add(OTHERS_ID, public_tip)};
        assert(!selfish_miner.ReceivedBlock(blocks, others_block));
        assert(selfish_miner.tip == others_block);
    }

    /** Case (f), lead was 1, others find a block. "Now there are two branches of length one, and the pool
     * publishes its single secret block." */
    {
        // A selfish miner with a 1-block private fork on top of a 2 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 1)};
        const BlockId selfish_block{selfish_miner.tip};

        // Now the selfish miner is notified of an equal-size best chain with the last block being the others'. He reveals
        // his last block and continues mining on top of it.
        const BlockId others_block{blocks.Add(OTHERS_ID, public_tip)};
        assert(selfish_miner.ReceivedBlock(blocks, others_block) == selfish_block);
        assert(selfish_miner.tip == selfish_block);
        assert(selfish_miner.withheld_blocks == 0);
    }

    /** Case (g), lead was 2, others find a block. "The others almost close the gap as the lead drops to 1.
     * The pool publishes its secret blocks, causing everybody to start mining at the head of the previously
     * private branch, since it is longer". */
    {
        // A selfish miner with a 2-blocks private fork on top of a 2 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 2)};
        const BlockId selfish_tip{selfish_miner.tip};

        // Now the selfish miner is notified of a best public chain with only one block less than his private chain.
        // He reveals all his private blocks.
        const BlockId others_block{blocks.Add(OTHERS_ID, public_tip)};
        assert(selfish_miner.ReceivedBlock(blocks, others_block) == selfish_tip);
        assert(selfish_miner.tip == selfish_tip);
        assert(selfish_miner.withheld_blocks == 0);
    }

    /** Case (h), lead was more than 2, others win. The others decrease the lead, which remains at least two.
     * The new block (say with number i) will end outside the chain once the pool publishes its entire branch. */
    {
        // A selfish miner with a 3-blocks private fork on top of a 2 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 3)};
        const BlockId selfish_tip{selfish_miner.tip};

        // Now the selfish miner is notified of a best public chain with two blocks less than his private chain. He reveals
        // the oldest block and keeps mining on its private fork.
        const BlockId others_block{blocks.Add(OTHERS_ID, public_tip)};
        const BlockId oldest_selfish_block{blocks.Ancestor(selfish_tip, 3)};
        assert(selfish_miner.ReceivedBlock(blocks, others_block) == oldest_selfish_block);
        assert(selfish_miner.tip == selfish_tip);
        assert(selfish_miner.withheld_blocks == 2);
    }
    {
        // A selfish miner with a 5-blocks private fork on top of a 3 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID, OTHERS_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 5)};
        const BlockId selfish_tip{selfish_miner.tip};

        // Now the selfish miner is notified of a best public chain with four blocks less than his private chain. He reveals
        // the oldest block and keeps mining on its private fork.
        const BlockId others_block{blocks.Add(OTHERS_ID, public_tip)};
        const BlockId oldest_selfish_block{blocks.Ancestor(selfish_tip, 4)};
        assert(selfish_miner.ReceivedBlock(blocks, others_block) == oldest_selfish_block);
        assert(selfish_miner.tip == selfish_tip);
        assert(selfish_miner.withheld_blocks == 4);
    }

    /** Case absent from the paper. Same as above but the rest of the network found two blocks in a row. */
    {
        // A selfish miner with a 5-blocks private fork on top of a 3 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID, OTHERS_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 5)};
        const BlockId selfish_tip{selfish_miner.tip};

        // Now the selfish miner is notified of a best public chain with three blocks less than his private chain. He
        // reveals the two oldest blocks and keeps mining on its private fork.
        const BlockId others_tip{AddBlocks(blocks, public_tip, {OTHERS_ID, OTHERS_ID})};
        const BlockId second_selfish_block{blocks.Ancestor(selfish_tip, 5)};
        assert(selfish_miner.ReceivedBlock(blocks, others_tip) == second_selfish_block);
        assert(selfish_miner.tip == selfish_tip);
        assert(selfish_miner.withheld_blocks == 3);
    }

    /** Case absent from the paper. Selfish miner has a 1-block lead and other miners find two blocks in a row. */
    {
        // A selfish miner with a 1-block private fork on top of a 3 blocks public chain.
        const BlockId public_tip{AddBlocks(blocks, GENESIS, {OTHERS_ID, SM_ID, OTHERS_ID})};
        Miner selfish_miner{SelfishMiner(blocks, SM_ID, public_tip, 1)};

        // Now the selfish miner is notified of a best public chain with 1 block more than his private one. He switches to it.
        const BlockId others_tip{AddBlocks(blocks, public_tip, {OTHERS_ID, OTHERS_ID})};
        assert(!selfish_miner.ReceivedBlock(blocks, others_tip));
        assert(selfish_miner.tip == others_tip);
        assert(selfish_miner.withheld_blocks == 0);
    }

    std::cout << "Selfish mining strategy tests passed." << std::endl;
}

/** Test a block is not counted as stale anymore if it makes it back to the best chain. */
void TestStaleBlocks()
{
    constexpr int MINER_ID{0}, OTHER_A{1}, OTHER_B{2};
    BlockTree blocks;
    Miner miner{MINER_ID, 30, 1s};

    // The miner finds a block but the 2-blocks chain of another miner is propagated first. Its block is stale.
    const BlockId miner_block{*miner.FoundBlock(blocks)};
    const BlockId other_chain{AddBlocks(blocks, GENESIS, {OTHER_A, OTHER_A})};
    miner.ReceivedBlock(blocks, other_chain);
    assert(miner.tip == other_chain);
    assert(miner.StaleBlocks(blocks) == 1);

    // A third miner kept mining on top of the miner's block, and now has the longest chain. Its block is not stale anymore.
    const BlockId third_chain{AddBlocks(blocks, miner_block, {OTHER_B, OTHER_B})};
    miner.ReceivedBlock(blocks, third_chain);
    assert(miner.tip == third_chain);
    assert(miner.StaleBlocks(blocks) == 0);

    std::cout << "Stale blocks accounting tests passed." << std::endl;
}

int main()
{
    TestSelfishStrategy();
    TestStaleBlocks();
    TestMinerPicker();
    TestBlockInterval();
    TestInstantPropagation();
    TestPropagationDelay();
    TestSelfishMining();
}
