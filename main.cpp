#include <iostream>
#include <future>

#include "simulation.h"

//! How long to run each simulation for.
static constexpr std::chrono::months SIM_DURATION{12};

//! How many simulations to run in parallel.
static constexpr int SIM_RUNS{16 * 2'048};

/** Set the hashrate distribution for the simulation. Must add up to 1. */
std::vector<Miner> SetupMiners()
{
    std::vector<Miner> miners;

    // Use hashrate data from https://mainnet.observer/charts/mining-pools-hashrate-distribution.
    // Assume homogenous propagation time (optimistic, as bigger pools are more likely to be better
    // connected). According to historical data from https://www.dsn.kastel.kit.edu/bitcoin the vast
    // majority of blocks reach relevant nodes within a second nowadays, improved from close to 10
    // seconds a decade ago.
    miners.emplace_back(0, 30, 1s); // Antpool & co. (Approximation)
    miners.emplace_back(1, 29, 1s); // Foundry. (Approximation)
    miners.emplace_back(2, 12, 1s); // ViaBTC. (Approximation)
    miners.emplace_back(3, 11, 1s); // F2pool. (Approximation)
    miners.emplace_back(4, 8, 1s); // Spider. (Approximation)
    miners.emplace_back(5, 5, 1s); // Mara. (Approximation)
    miners.emplace_back(6, 3, 1s); // Secpool. (Approximation)
    // Some made-up small miners
    miners.emplace_back(7, 1, 1s);
    miners.emplace_back(8, 1, 1s);

    return miners;
}

/** Run the simulation SIM_RUNS times for SIM_DURATION with the network configuration defined in SetupMiners(). */
int main()
{
    const auto miners{SetupMiners()};
    // NOTE: hardware_concurrency() returns 0 if it cannot tell.
    const auto thread_count{std::max(1u, std::thread::hardware_concurrency())};
    std::vector<MinerStats> stats_total(miners.size());

    std::cout << "Running " << SIM_RUNS << " simulations in parallel using " << thread_count << " threads." << std::endl;

    // Run the simulation `SIM_RUNS` times by batches of at most `thread_count` parallel tasks. Record
    // all stats into `stats_total`. Each simulation is given its own random 64-bit seed.
    std::random_device rd;
    unsigned int remaining_tasks{SIM_RUNS};
    while (remaining_tasks > 0) {
        // The last batch is smaller if `SIM_RUNS` is not a multiple of `thread_count`.
        const auto batch_size{std::min(thread_count, remaining_tasks)};
        std::vector<std::future<std::vector<MinerStats>>> sim_futures;
        for (unsigned int i{0}; i < batch_size; ++i) {
            const uint64_t seed{(uint64_t{rd()} << 32) | rd()};
            sim_futures.emplace_back(std::async(std::launch::async, &RunSimulation, SIM_DURATION, miners, seed));
        }
        for (auto& fut: sim_futures) {
            const auto stats{fut.get()};
            assert(stats.size() == stats_total.size());
            for (int j{0}; j < stats.size(); ++j) {
                stats_total[j] += stats[j];
            }
        }
        remaining_tasks -= batch_size;
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
