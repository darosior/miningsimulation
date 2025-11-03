#include <iostream>

#include "simulation.h"

//! How often to print statistics.
static constexpr std::chrono::seconds PRINT_INTERVAL{BLOCK_INTERVAL * 144};

/** Statistics about a miner's revenue in function of the best chain. */
struct MinerStats {
    //! The count of blocks found by this miner in the best chain.
    long blocks_found;
    //! The ratio of blocks found by this miner in the best chain.
    double blocks_share;
    //! The ratio of blocks found in the best chain over stale blocks for this miner.
    double stale_rate;

    /** Compute revenue statistics for this miner in the given best chain. */
    explicit MinerStats(const Miner& miner, std::span<const Block> best_chain)
    {
        blocks_found = std::count_if(best_chain.begin(), best_chain.end(), [&miner](const Block& b) {
            return b.miner_id == miner.id;
        });
        // -1 for the genesis block.
        blocks_share = blocks_found == 0 ? 0.0 : static_cast<double>(blocks_found) / (best_chain.size() - 1);
        stale_rate = blocks_found == 0 ? 0.0 : static_cast<double>(miner.stale_blocks) / blocks_found;
    }
};

/** Set the hashrate distribution for the simulation. Must add up to 1. */
std::vector<Miner> SetupMiners()
{
    std::vector<Miner> miners;

    // Use hashrate data from https://mainnet.observer/charts/mining-pools-hashrate-distribution.
    // Assume homogenous propagation time (optimistic, as bigger pools are more likely to be better
    // connected). We choose the propagation time according to historical data from
    // https://www.dsn.kastel.kit.edu/bitcoin. For the deteriorated propagation case we choose 20s
    // (probably on the pessimistic end).
    miners.emplace_back(0, 30, 20s); // Antpool & co.
    miners.emplace_back(1, 29, 20s); // Foundry.
    miners.emplace_back(2, 12, 20s); // ViaBTC.
    miners.emplace_back(3, 11, 20s); // F2pool.
    miners.emplace_back(4, 8, 20s); // Spider.
    miners.emplace_back(5, 5, 20s); // Mara.
    miners.emplace_back(6, 3, 20s); // Secpool.
    // Some made-up small miners
    miners.emplace_back(7, 1, 20s);
    miners.emplace_back(8, 1, 20s);

    return miners;
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

    // Set the hashrate distribution for the simulation.
    auto miners{SetupMiners()};

    // Run the simulation. As we advance time, we check if a block was found, and if so which miner
    // found it. We also check if any miner needs to reorg once one miner's chain reached it.
    size_t best_chain_size{1};
    for (std::chrono::milliseconds cur_time{0}; ; cur_time += 1ms) {
        // Has a block been found by now? NOTE: `while` and not `if` in the unlikely case that
        // NextBlockInterval() returns 0.
        while (cur_time == next_block_time) {
            Miner& miner{PickFinder(miners, miner_picker)};
            miner.FoundBlock(next_block_time, best_chain_size);
            next_block_time += NextBlockInterval(block_interval);
        }
        assert(cur_time < next_block_time); // Must never miss any as we advance in steps of 1ms.

        // Record the best propagated chain among all miners, and let them all know about it. They
        // might switch to it if it's longer or act upon the information (for instance a selfish miner
        // may selectively reveal some of its private blocks). Among chains of the same size, pick
        // the one which arrived first (matching Bitcoin Core's first-seen rule).
        std::span<const Block> best_chain;
        for (const auto& miner: miners) {
            const auto pub_chain{miner.PublishedChain(cur_time)};
            const bool more_work{pub_chain.size() > best_chain.size()};
            const bool first_seen{pub_chain.size() == best_chain.size() && !pub_chain.empty() && pub_chain.back().arrival < best_chain.back().arrival};
            if (more_work || first_seen) {
                best_chain = pub_chain;
            }
        }
        for (auto& miner: miners) {
            miner.NotifyBestChain(best_chain, cur_time);
        }

        // Record the best chain size as FoundBlock() may decide not to publish a block based on this
        // information.
        best_chain_size = best_chain.size();

        // Print some stats about each miner from time to time.
        if (cur_time > 0s && cur_time % PRINT_INTERVAL == 0s) {
            const auto sec{std::chrono::duration_cast<std::chrono::seconds>(cur_time)};
            const auto days{std::chrono::duration_cast<std::chrono::days>(cur_time)};
            const auto total_blocks{best_chain.size() - 1};
            std::cout << "After " << sec << " (" << days << ") and " << total_blocks << " blocks found:" << std::endl;
            for (const auto& miner: miners) {
                const auto stats{MinerStats(miner, best_chain)};
                std::cout << "  - Miner " << miner.id << " (" << miner.perc << "% of network hashrate) found " << stats.blocks_found << " blocks i.e. ";
                std::cout << stats.blocks_share * 100 << "% of blocks. Stale rate: " << stats.stale_rate * 100 << "%.";
                if (miner.is_selfish) std::cout << " ('selfish mining' strategy)";
                std::cout << std::endl;
            }
            std::cout << std::endl << std::endl;
        }
    }
}
