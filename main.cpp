#include <algorithm>
#include <cassert>
#include <chrono>
#include <optional>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <ranges>
#include <span>

#include "randomsipa.h"

using namespace std::chrono_literals;

//! Expected time between blocks. Used as parameter for the exponential distribution we are sampling from.
static constexpr std::chrono::milliseconds BLOCK_INTERVAL{600'000};
//! How often to print statistics.
static constexpr std::chrono::milliseconds PRINT_FREQ{BLOCK_INTERVAL * 144};
//! We use integers in [0;100] for percentages. This is the multiplier to map them to [0; uint64_t::MAX].
static constexpr uint64_t PERC_MULTIPLIER{std::numeric_limits<uint64_t>::max() / 100};
//! Arrival time to use for unpublished blocks by a selfish miner.
static constexpr std::chrono::milliseconds SELFISH_ARRIVAL{std::chrono::milliseconds::max()};

struct Block {
    //! Which miner created this block.
    unsigned miner_id;
    //! At what point will all other miners receive this block.
    std::chrono::milliseconds arrival;

    explicit Block(unsigned id, std::chrono::milliseconds time): miner_id{id}, arrival{time} {}

    //! Create the genesis block, not created by any miner and always received immediately.
    static Block Genesis() {
        return Block(std::numeric_limits<unsigned>::max(), 0s);
    }

    bool operator==(const Block& other) {
        return miner_id == other.miner_id && arrival == other.arrival;
    }
};

struct Miner {
    //! Miner identifier used to track which miner created a certain block.
    unsigned id;
    //! Share of the total network hashrate controlled by the miner, as an integer between 0 and 100.
    uint64_t perc;
    //! The time for blocks produced by this miner to reach all other miners.
    std::chrono::milliseconds propagation;
    //! Local chain on the miner's full node. May differ slightly between miners due to propagation time.
    std::vector<Block> chain;
    //! The next time this miner will find a block, sampled from an exponential distribution parameterized by its share of total network hashrate.
    std::chrono::milliseconds next_block;
    //! Number of blocks this miner created that were reorged out.
    int stale_blocks;
    //! Whether this miner follows a (worst case) selfish mining strategy as described in section 3.2 of https://arxiv.org/pdf/1311.0243.
    bool is_selfish;

    explicit Miner(unsigned id_, uint64_t perc_, std::chrono::milliseconds prop, std::random_device& rd, bool selfish = false)
        : id{id_}, perc{perc_}, propagation{prop}, chain{{Block::Genesis()}}, stale_blocks{0}, is_selfish{selfish}
    {}

    /** Add a block found at the given block time to this miner's local chain. */
    void FoundBlock(std::chrono::milliseconds block_time, std::span<const Block> best_chain) {
        if (is_selfish) {
            // A selfish miner always mines on top of its private chain, except in the case of a 1-block
            // race whereby if he wins the race he'll publish both blocks.
            const bool is_race{SelfishBlocks() == 1 && best_chain.size() == chain.size()};
            if (is_race) {
                chain.back().arrival = block_time + propagation;
                chain.emplace_back(id, block_time + propagation);
            } else {
                chain.emplace_back(id, SELFISH_ARRIVAL);
            }
        } else {
            chain.emplace_back(id, block_time + propagation);
        }
    }

    /** Count the number of not-yet-propagated blocks in this miner's local chain. */
    int UnpublishedBlocks(std::chrono::milliseconds cur_time) const {
        int unpublished_blocks{0};
        for (const auto& block: std::views::reverse(chain)) {
            // Arrival time is monotonic, don't bother doing useless work.
            if (block.arrival <= cur_time) {
                break;
            }
            unpublished_blocks++;
        }
        return unpublished_blocks;
    }

    /** Length of a selfish miner's private branch. Called `privateBranchLen` in the paper's algorithm. */
    size_t SelfishBlocks() const {
        size_t selfish_blocks{0};
        for (const auto& block: std::views::reverse(chain)) {
            // Selfish blocks are always ever at the end of the chain.
            if (block.arrival != SELFISH_ARRIVAL) {
                break;
            }
            ++selfish_blocks;
        }
        return selfish_blocks;
    }

    /** Get the chain from this miner, except for the block that were not yet propagated. */
    std::span<const Block> PublishedChain(std::chrono::milliseconds cur_time) const {
        int unpublished_blocks{UnpublishedBlocks(cur_time)};
        return std::span{chain}.first(chain.size() - unpublished_blocks);
    }

    /** Replace our chain if another miner's fully-propagated one is longer. */
    void MaybeReorg(std::span<const Block> best_chain) {
        // Of course we assume all blocks are at the same difficulty.
        if (best_chain.size() <= chain.size()) return;

        // It's really only the last couple of blocks that may ever change, so try
        // to be smart and don't wipe and reallocate the whole vector every time.
        for (size_t i{0}; i < best_chain.size(); ++i) {
            if (i > chain.size() - 1) {
                chain.push_back(best_chain[i]);
            } else if (chain[i] != best_chain[i]) {
                // This block was reorged out. If it's ours, update the stale block counter.
                if (chain[i].miner_id == id) stale_blocks++;
                chain[i] = best_chain[i];
            }
            // else: same block at same height.
        }
    }

    /** If this miner follows the selfish mining strategy, choose whether to selectively reveal some
     * blocks. The strategy implemented here follows the one described in the 2013 "Majority is not enough"
     * research paper in the worst case scenario, ie Gamma=0 (in the case of a 1-block race no other miner
     * mines on top of a selfish miner's block). Paper available at https://arxiv.org/pdf/1311.0243.
     */
    void MaybeSelfishReveal(std::span<const Block> best_chain, std::chrono::milliseconds cur_time) {
        if (!is_selfish) return;

        // If their chain is already longer than ours, we have to switch. The selfish blocks will be
        // overwritten by MaybeReorg().
        if (best_chain.size() > chain.size()) return;

        // If our chain is still at least the same size, we keep mining on it. Note that even when they
        // are the same size, we may be mining on top of a different block still in the case of a 1-block
        // race.
        // If they are catching up, reveal as many blocks as they have just found.
        const size_t selfish_count{SelfishBlocks()};
        const size_t current_lead{chain.size() - best_chain.size()};
        if (selfish_count > current_lead) {
            size_t reveal_count{selfish_count - current_lead};
            // Special case: if we had a significant lead and they are almost caught up reveal everything
            // now to avoid a race.
            if (selfish_count > 1 && current_lead == 1) {
                reveal_count = selfish_count;
            }
            // Broadcast as many blocks as necessary by setting their arrival time.
            for (size_t i{0}; i < reveal_count; ++i) {
                chain.at(chain.size() - selfish_count + i).arrival = cur_time + propagation;
            }
        }
    }

    /** Let this miner know about the longest published chain. */
    void NotifyBestChain(std::span<const Block> best_chain, std::chrono::milliseconds cur_time) {
        MaybeSelfishReveal(best_chain, cur_time);
        MaybeReorg(best_chain);
    }

    /** Count of published blocks found by this miner. */
    long BlocksFound(std::chrono::milliseconds cur_time) const {
        return std::count_if(chain.begin(), chain.end(), [&](const Block& b) {
            return b.miner_id == id && b.arrival <= cur_time;
        });
    }

    /** Compute the share of (published) blocks found by this miner. */
    double BlocksFoundShare(std::chrono::milliseconds cur_time) const {
        size_t published_blocks{chain.size() - UnpublishedBlocks(cur_time)};
        long found_blocks{BlocksFound(cur_time)};
        return static_cast<double>(found_blocks) / (published_blocks - 1); // -1 for the genesis
    }

    /** Proportion of stale blocks per block found by this miner. */
    double StaleRate(std::chrono::milliseconds cur_time) const {
        long found_blocks{BlocksFound(cur_time)};
        if (found_blocks == 0) return 0.0;
        return static_cast<double>(stale_blocks) / found_blocks;
    }
};

/** Draw the time between the last and the next block from the given exponential distribution. */
std::chrono::milliseconds NextBlockInterval(RNG& rng)
{
    const double exporand{std::round(rng.exporand(BLOCK_INTERVAL.count()))};
    assert(exporand >= 0.0); // Must not go backward.
    return std::chrono::milliseconds(static_cast<long>(exporand));
}

/** Pick which miner found the last block based on its hashrate and a uniform distribution. */
Miner& PickFinder(std::vector<Miner>& miners, RNG& rng)
{
    uint64_t random{rng.rand64()}, i{0};
    for (auto& miner: miners) {
        i += miner.perc * PERC_MULTIPLIER;
        if (i > random) return miner;
    }
    assert(!"The miners' percentages must add up to 100.");
}

/** Simulate the Bitcoin mining process with a given number of miners, each with a given share of the
 * network hashrate and with a given block propagation time.
 *
 * The propagation time is a simplification: it is the time before which a miner's block has not reached
 * any other miner and after which it has reached all other miners.
 *
 * The mining process is accurately modeled: we draw the time between the last and next block from an
 * exponential distribution, then draw which miner found this block based on its hashrate and a uniform
 * distribution. Difficulty and network hashrate are assumed to be constant.
 *
 * This assumes today's Bitcoin Core behaviour: a miner will mine on top of its own block immediately and will
 * only switch to a propagated chain if its longer (again, difficulty is assumed constant).
 */
int main()
{
    // Create a number of miners with a given set of parameters.
    std::random_device rd;
    // Random number generators used to respectively pick the time before the next block interval and
    // pick which miner found the last block.
    RNG block_interval{rd()}, miner_picker{rd()};

    // Absolute time of the next block arrival. Since we are starting from 0, for the first one this is
    // just the block interval itself.
    std::chrono::milliseconds next_block_time{NextBlockInterval(block_interval)};

    // Create our set of miners. The share of network hashrate must add up to 1.
    std::vector<Miner> miners;
    miners.emplace_back(0, 10, 100ms, rd);
    miners.emplace_back(1, 15, 100ms, rd);
    miners.emplace_back(2, 15, 100ms, rd);
    miners.emplace_back(3, 20, 100ms, rd);
    miners.emplace_back(4, 40, 100ms, rd, true);

    // Run the simulation. As we advance time, we check if a block was found, and if so which miner
    // found it. We also check if any miner needs to reorg once one miner's chain reached it.
    std::span<const Block> best_chain;
    for (std::chrono::milliseconds cur_time{0}; ; cur_time += 1ms) {
        // Has a block been found by now? NOTE: `while` and not `if` in the unlikely case that
        // NextBlockInterval() returns 0.
        while (cur_time == next_block_time) {
            Miner& miner{PickFinder(miners, miner_picker)};
            miner.FoundBlock(next_block_time, best_chain);
            next_block_time += NextBlockInterval(block_interval);
        }
        assert(cur_time < next_block_time); // Must never miss any as we advance in steps of 1ms.

        // Record the best propagated chain among all miners, and let them all know about it. They
        // might switch to it if it's longer or act upon the information (for instance a selfish miner
        // may selectively reveal some of its private blocks). Among chains of the same size, pick
        // the one which arrived first (matching Bitcoin Core's first-seen rule).
        for (const auto& miner: miners) {
            const auto pub_chain{miner.PublishedChain(cur_time)};
            const bool more_work{pub_chain.size() > best_chain.size()};
            const bool first_seen{pub_chain.size() == best_chain.size() && !pub_chain.empty() && !best_chain.empty() && pub_chain.back().arrival < best_chain.back().arrival};
            if (more_work || first_seen) {
                best_chain = pub_chain;
            }
        }
        for (auto& miner: miners) {
            miner.NotifyBestChain(best_chain, cur_time);
        }

        // Print some stats about each miner from time to time.
        if (cur_time > 0s && cur_time % PRINT_FREQ == 0s) {
            const auto total_blocks{best_chain.size() - 1};
            std::cout << "After " << std::chrono::duration_cast<std::chrono::seconds>(cur_time) << " (" << std::chrono::duration_cast<std::chrono::days>(cur_time) << ") and " << total_blocks << " blocks found:" << std::endl;
            for (const auto& miner: miners) {
                const auto blocks_share{miner.BlocksFoundShare(cur_time)};
                const auto stale_rate{miner.StaleRate(cur_time)};
                std::cout << "  - Miner " << miner.id << " (" << miner.perc << "% of network hashrate) found " << miner.BlocksFound(cur_time) << " blocks i.e. ";
                std::cout << blocks_share * 100 << "% of blocks. Stale rate: " << stale_rate * 100 << "%.";
                if (miner.is_selfish) std::cout << " ('selfish mining' strategy)";
                std::cout << std::endl;
            }
            std::cout << std::endl << std::endl;
        }
    }
}
