#include <iostream>
#include <future>

#include "simulation.h"

//! How long to run each simulation for.
static constexpr std::chrono::months SIM_DURATION{12};

//! How many simulations to run in parallel.
static constexpr int SIM_RUNS{16 * 2'048};

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

    explicit MinerStats(): blocks_found{0}, blocks_share{0.0}, stale_rate{0.0} {}

    MinerStats& operator+=(const MinerStats& other)
    {
        blocks_found += other.blocks_found;
        blocks_share += other.blocks_share;
        stale_rate += other.stale_rate;
        return *this;
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

/** Get the best chain known by any miner. */
std::span<const Block> BestChain(const std::vector<Miner>& miners, std::chrono::milliseconds cur_time)
{
    std::span<const Block> best_chain;

    for (const auto& miner: miners) {
        const auto pub_chain{miner.PublishedChain(cur_time)};
        const bool more_work{pub_chain.size() > best_chain.size()};
        const bool first_seen{pub_chain.size() == best_chain.size() && !pub_chain.empty() && pub_chain.back().arrival < best_chain.back().arrival};
        if (more_work || first_seen) {
            best_chain = pub_chain;
        }
    }

    return best_chain;
}

/** Has any miner published a block that was not yet received by the entirety of the network? */
bool AnyBlockInFlight(const std::vector<Miner>& miners, std::chrono::milliseconds cur_time)
{
    for (const auto& miner: miners) {
        const auto unpublished_blocks{miner.UnpublishedBlocks(cur_time)};
        if (!miner.is_selfish && unpublished_blocks > 0) {
            return true;
        } else if (miner.is_selfish && unpublished_blocks > miner.SelfishBlocks()) {
            return true;
        }
    }
    return false;
}

/** The earliest next block to arrive, if any. */
std::optional<std::chrono::milliseconds> EarliestArrival(const std::vector<Miner>& miners, std::chrono::milliseconds cur_time)
{
    std::optional<std::chrono::milliseconds> earliest_arrival{};
    for (const auto& miner: miners) {
        if (const auto next_arrival = miner.NextArrival(cur_time)) {
            if (earliest_arrival.has_value()) {
                earliest_arrival = std::min(earliest_arrival.value(), next_arrival.value());
            } else {
                earliest_arrival = next_arrival;
            }
        }
    }
    return earliest_arrival;
}

/** Simulate the Bitcoin mining process for a given amount of time with the given miners, each having its
 * own share of network hashrate and block propagation time.
 *
 * The propagation time is a simplification: it is the time before which a miner's block has not reached
 * any other miner and after which it has reached all other miners.
 *
 * The mining process is accurately modeled: we draw the time between the last and next block from an
 * exponential distribution, then draw which miner found this block based on its hashrate and a uniform
 * distribution. Difficulty and network hashrate are assumed to be constant.
 *
 * This assumes today's Bitcoin Core behaviour: a miner will mine on top of its own block immediately and will
 * only switch to a propagated chain if it's longer (again, difficulty is assumed constant). Miners can optionally
 * be set to adopt the "selfish mining" strategy in SetupMiners().
 */
std::vector<MinerStats> RunSimulation(std::chrono::milliseconds duration_time, std::vector<Miner> miners)
{
    // Create a number of miners with a given set of parameters.
    std::random_device rd;
    // Random number generators used to respectively pick the time before the next block interval and
    // pick which miner found the last block.
    RNG block_interval{rd()}, miner_picker{rd()};

    // Absolute time of the next block arrival. Since we are starting from 0, for the first one this is
    // just the block interval itself.
    std::chrono::milliseconds next_block_time{NextBlockInterval(block_interval)};

    std::chrono::milliseconds mean_prop_time{0};
    for (const auto& miner: miners) {
        mean_prop_time += miner.propagation;
    }
    mean_prop_time /= miners.size();
    mean_prop_time = std::max(1ms, mean_prop_time);

    // Run the simulation. As we advance time, we check if a block was found, and if so which miner
    // found it. We also check if any miner needs to reorg once one miner's chain reached it.
    size_t best_chain_size{1};
    for (std::chrono::milliseconds cur_time{0}; cur_time < duration_time; ) {
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
        const auto best_chain{BestChain(miners, cur_time)};
        for (auto& miner: miners) {
            miner.NotifyBestChain(best_chain, cur_time);
        }

        // Record the best chain size as FoundBlock() may decide not to publish a block based on this
        // information.
        best_chain_size = best_chain.size();

        // There is only two events that may change the state of miners (including the selfish ones):
        // a block is found, or a block is received. Instead of iterating through every ms where nothing
        // will happen, cut-through to the next event.
        const auto earliest_arrival{EarliestArrival(miners, cur_time)};
        cur_time = next_block_time;
        if (earliest_arrival.has_value()) {
            if (earliest_arrival.value() < cur_time) {
                cur_time = earliest_arrival.value();
            }
        }
    }

    const auto best_chain{BestChain(miners, duration_time)};
    std::vector<MinerStats> stats;
    for (const auto& miner: miners) {
        stats.emplace_back(MinerStats(miner, best_chain));
    }

    return stats;
}

/** Run the simulation SIM_RUNS times for SIM_DURATION with the network configuration defined in SetupMiners(). */
int main()
{
    const auto miners{SetupMiners()};
    const auto thread_count{std::thread::hardware_concurrency()};
    std::vector<MinerStats> stats_total(miners.size());

    std::cout << "Running " << SIM_RUNS << " simulations in parallel using " << thread_count << " threads." << std::endl;

    // Run the simulation `SIM_RUNS` times by batches of `thread_count` parallel tasks. Record all stats
    // into `stats_total`.
    unsigned int remaining_tasks{SIM_RUNS};
    while (remaining_tasks > 0) {
        std::vector<std::future<std::vector<MinerStats>>> sim_futures;
        for (int i{0}; i < std::min(thread_count, remaining_tasks); ++i) {
            sim_futures.emplace_back(std::async(std::launch::async, &RunSimulation, SIM_DURATION, miners));
        }
        for (auto& fut: sim_futures) {
            const auto stats{fut.get()};
            assert(stats.size() == stats_total.size());
            for (int j{0}; j < stats.size(); ++j) {
                stats_total[j] += stats[j];
            }
        }
        remaining_tasks -= thread_count;
        std::cout << '\r' << (SIM_RUNS - remaining_tasks) * 100 / SIM_RUNS << "% progress..";
    }
    std::cout << std::endl;

    // Print the stats for each miner by averaging over all simulation runs.
    const auto days{std::chrono::duration_cast<std::chrono::days>(SIM_DURATION)};
    std::cout << "After running " << SIM_RUNS << " simulations for " << days << " each, on average:" << std::endl;
    assert(miners.size() == stats_total.size());
    for (int i{0}; i < miners.size(); ++i) {
        const auto& miner{miners[i]};
        const auto& stats{stats_total[i]};
        std::cout << "  - Miner " << miner.id << " (" << miner.perc << "% of network hashrate) found " << stats.blocks_found / SIM_RUNS << " blocks i.e. ";
        std::cout << stats.blocks_share * 100 / SIM_RUNS << "% of blocks. Stale rate: " << stats.stale_rate * 100 / SIM_RUNS << "%.";
        if (miner.is_selfish) std::cout << " ('selfish mining' strategy)";
        std::cout << std::endl;
    }
}
