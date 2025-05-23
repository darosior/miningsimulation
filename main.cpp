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

    explicit Miner(unsigned id_, uint64_t perc_, std::chrono::milliseconds prop, std::random_device& rd)
        : id{id_}, perc{perc_}, propagation{prop}, chain{{Block::Genesis()}}, stale_blocks{0}
    {}

    /** Add a block found at the given block time to this miner's local chain. */
    void FoundBlock(std::chrono::milliseconds block_time) {
        chain.emplace_back(id, block_time + propagation);
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
                chain[i] = best_chain[i];
                // This block was reorged out. If it's ours, update the stale block counter.
                if (chain[i].miner_id == id) stale_blocks++;
            }
            // else: same block at same height.
        }
    }

    /** Compute the share of (published) blocks found by this miner. */
    double BlocksFoundShare(std::chrono::milliseconds cur_time) const {
        size_t published_blocks{chain.size() - UnpublishedBlocks(cur_time)};
        long found_blocks{std::count_if(chain.begin(), chain.end(), [&](const Block& b) {
            return b.miner_id == id && b.arrival <= cur_time;
        })};
        return static_cast<double>(found_blocks) / (published_blocks - 1); // -1 for the genesis
    }

    double StaleRate(std::chrono::milliseconds cur_time) const {
        long found_blocks{std::count_if(chain.begin(), chain.end(), [&](const Block& b) {
            return b.miner_id == id && b.arrival <= cur_time;
        })};
        return static_cast<double>(stale_blocks) / found_blocks;
    }
};

/** Draw the time between the last and the next block from the given exponential distribution. */
std::chrono::milliseconds NextBlockInterval(RNG& rng)
{
    return std::chrono::milliseconds(static_cast<long>(std::round(rng.exporand(BLOCK_INTERVAL.count())))); // FIXME: check precision
}

/** Pick which miner found the last block based on its hashrate and a uniform distribution. */
Miner& PickFinder(std::vector<Miner>& miners, RNG& rng)
{
    uint64_t random{rng.rand64()}, i{0};
    for (auto& miner: miners) {
        i += miner.perc * PERC_MULTIPLIER;
        if (i >= random) return miner;
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
    miners.emplace_back(0, 10, 100s, rd);
    miners.emplace_back(1, 20, 100s, rd);
    miners.emplace_back(2, 20, 100s, rd);
    miners.emplace_back(3, 20, 100s, rd);
    miners.emplace_back(4, 30, 100s, rd);

    // Run the simulation. As we advance time, we check if a block was found, and if so which miner
    // found it. We also check if any miner needs to reorg once one miner's chain reached it.
    for (std::chrono::milliseconds cur_time{0}; ; cur_time += 1ms) {
        // Has a block been found by now?
        if (cur_time == next_block_time) {
            Miner& miner{PickFinder(miners, miner_picker)};
            miner.FoundBlock(next_block_time);
            next_block_time += NextBlockInterval(block_interval);
        } else {
            assert(cur_time < next_block_time); // Must not have missed one.
        }

        // Check if any miner needs to change the chain it is mining on. That is, if any other miner's
        // chain is longer and has propagated.
        std::span<const Block> best_chain;
        for (const auto& miner: miners) {
            const auto pub_chain{miner.PublishedChain(cur_time)};
            if (pub_chain.size() > best_chain.size()) {
                best_chain = pub_chain;
            }
        }
        for (auto& miner: miners) {
            miner.MaybeReorg(best_chain);
        }

        // Print some stats about each miner from time to time.
        if (cur_time > 0s && cur_time % PRINT_FREQ == 0s) {
            std::cout << "After " << std::chrono::duration_cast<std::chrono::seconds>(cur_time) << " (" << std::chrono::duration_cast<std::chrono::days>(cur_time) << "):" << std::endl;
            for (const auto& miner: miners) {
                const auto blocks_share{miner.BlocksFoundShare(cur_time)};
                const auto stale_rate{miner.StaleRate(cur_time)};
                std::cout << "  - Miner " << miner.id << " (" << miner.perc << "% of network hashrate) found ";
                std::cout << blocks_share * 100 << "% of blocks. Stale rate: " << stale_rate * 100 << "%." << std::endl;
            }
            std::cout << std::endl << std::endl;
        }
    }
}
