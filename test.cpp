#include <algorithm>
#include <cmath>
#include <concepts>
#include <iomanip>
#include <iostream>
#include <map>
#include <ranges>

#include "simulation.h"

// Analyze a sample of the distribution of blocks found per miners, to check we are indistinguishable from
// the expected distribution. Here we generate a 100 millions blocks with a 100 miners each with 1% of the
// network hashrate. The number of blocks found by a miner is a binomial distribution with p=0.01 and n=100 million.
// We therefore expect a sample mean of 1 million and a standard deviation of 1000. This requires libc++ to compile.
void MinerPickerSample()
{
    std::random_device rd;
    RNG rng{rd()};
    constexpr int TOTAL_BLOCK_COUNT{100'000'000};

    std::vector<Miner> miners;
    for (int i{0}; i < 100; ++i) {
        miners.emplace_back(i, 1, 0s);
        miners.back().chain.pop_back(); // drop the genesis block
        miners.back().chain.reserve((TOTAL_BLOCK_COUNT + 99) / 100);
    }

    for (int i{0}; i < TOTAL_BLOCK_COUNT; ++i) {
        auto& miner{PickFinder(miners, rng)};
        miner.FoundBlock(0ms, 0);
    }

    double mean{0.0}, squared_mean{0.0};
    std::map<size_t, int> block_counts;
    for (const auto& miner: miners) {
        const auto block_count{miner.chain.size()};
        mean += block_count;
        squared_mean += block_count * block_count;
        auto pair{block_counts.try_emplace(block_count, 0)};
        pair.first->second++;
    }
    mean /= static_cast<double>(miners.size());
    squared_mean /= static_cast<double>(miners.size());

    const double variance{squared_mean - mean * mean};
    std::vector sorted_block_counts = miners | std::views::transform([](const Miner& m){ return m.chain.size(); }) | std::ranges::to<std::vector<double>>();
    std::ranges::sort(sorted_block_counts);
    const double median{(sorted_block_counts[48] + sorted_block_counts[49]) / 2.0};
    std::cout << std::fixed << "Mean " << mean << ", std dev " << std::sqrt(variance) << ", median " << median << std::endl;

    /**
    assert(block_counts.size() <= miners.size());
    std::cout << "Number of miners with different block counts: " << block_counts.size() << std::endl;

    std::cout << "Histogram:" << std::endl;
    for (const auto& [block_count, miner_count]: block_counts) {
        const auto perc{static_cast<double>(block_count) / TOTAL_BLOCK_COUNT * 100};
        std::cout << std::fixed << std::setprecision(4) << block_count << " (" << perc << "%):";
        for (int i{0}; i < miner_count; ++i) std::cout << " *";
        std::cout << std::endl;
    }
    */
}

// Analyze the sample mean for many sample drawn from the distribution of blocks with miners with
// different hashrate. This is to make sure the way we sample from the distribution is not skewed
// with hashrate somehow.
void MinerPickerSmallBig()
{
    std::random_device rd;
    RNG rng{rd()};
    constexpr int SAMPLE_COUNT{10'000};
    constexpr int SAMPLE_SIZE{1'000};
    constexpr int TOTAL_BLOCK_COUNT{100};

    std::vector<Miner> miners;
    miners.emplace_back(0, 12, 0s);
    miners.emplace_back(1, 18, 0s);
    miners.emplace_back(2, 20, 0s);
    miners.emplace_back(3, 15, 0s);
    miners.emplace_back(4, 35, 0s);
    for (auto& miner: miners) {
        miner.chain.reserve((TOTAL_BLOCK_COUNT + 99) / 100);
        miner.chain.pop_back(); // drop the genesis block
    }

    std::vector<double> sample_means(miners.size()), sample_squared_means(miners.size());
    for (int k{0}; k < SAMPLE_COUNT; ++k) {
        std::vector<double> means(miners.size());
        for (int j{0}; j < SAMPLE_SIZE; ++j) {
            for (int i{0}; i < TOTAL_BLOCK_COUNT; ++i) {
                auto& miner{PickFinder(miners, rng)};
                miner.FoundBlock(0ms, 0);
            }

            for (size_t i{0}; i < miners.size(); ++i) {
                const auto block_count{miners[i].chain.size()};
                means[i] += block_count;
                miners[i].chain.clear();
            }
        }

        for (size_t i{0}; i < miners.size(); ++i) {
            means[i] /= SAMPLE_SIZE;
            sample_means[i] += means[i];
            sample_squared_means[i] += std::pow(means[i], 2);
        }
    }

    for (size_t i{0}; i < miners.size(); ++i) {
        sample_means[i] /= SAMPLE_COUNT;
        const auto sample_mean_perc{sample_means[i] / TOTAL_BLOCK_COUNT * 100};
        sample_squared_means[i] /= SAMPLE_COUNT;
        const double std_dev{std::sqrt(sample_squared_means[i] - std::pow(sample_means[i], 2))};
        const double std_dev_perc{std_dev / TOTAL_BLOCK_COUNT * 100};
        std::cout << std::fixed << "Miner " << miners[i].id << " with " << miners[i].perc << "% of the hashrate: ";
        std::cout << std::fixed << "sample mean " << sample_means[i] << " (" << sample_mean_perc << "%), std dev of sample mean " << std_dev << " (" << std_dev_perc << "%)" << std::endl;
    }
}

/* Run the simulation from main.cpp 100 times for two weeks. Take a 100 such samples. Compute the sample mean of the distribution of blocks found per miner. */
void SimpleSim()
{
    constexpr int SAMPLE_COUNT{100};
    constexpr int SAMPLE_SIZE{100};
    static constexpr std::chrono::milliseconds SIM_DURATION{BLOCK_INTERVAL * 144 * 14};

    std::vector<Miner> miners;
    miners.emplace_back(0, 12, 0s);
    miners.emplace_back(1, 18, 0s);
    miners.emplace_back(2, 20, 0s);
    miners.emplace_back(3, 15, 0s);
    miners.emplace_back(4, 35, 0s);

    std::vector<double> sample_means(miners.size()), sample_squared_means(miners.size());
    for (int counter1{0}; counter1 < SAMPLE_COUNT; ++counter1) {
        std::vector<double> means(miners.size());
        for (int counter2{0}; counter2 < SAMPLE_SIZE; ++counter2) {
            // Reproduce a simplified version of the simulation in main.cpp. No selfish mining and steps of 1s.
            std::random_device rd;
            RNG block_interval{rd()}, miner_picker{rd()};
            std::chrono::milliseconds next_block_time{NextBlockInterval(block_interval)};
            for (std::chrono::milliseconds cur_time{0}; cur_time < SIM_DURATION; cur_time += 1s) {
                while (cur_time >= next_block_time) {
                    Miner& miner{PickFinder(miners, miner_picker)};
                    miner.FoundBlock(next_block_time, /*best_chain_size=*/0); // best chain size 0 since no selfish mining
                    next_block_time += NextBlockInterval(block_interval);
                }

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
            }

            for (size_t i{0}; i < miners.size(); ++i) {
                means[i] += miners[i].BlocksFoundShare(SIM_DURATION);
                miners[i].chain.clear();
            }
        }

        for (size_t i{0}; i < miners.size(); ++i) {
            means[i] /= SAMPLE_SIZE;
            sample_means[i] += means[i];
            sample_squared_means[i] += std::pow(means[i], 2);
        }

        // Show some progress as the runtime is still pretty long
        std::cout << counter1 * 100 / SAMPLE_COUNT << "%" << "\r" << std::flush;
    }

    for (size_t i{0}; i < miners.size(); ++i) {
        sample_means[i] /= SAMPLE_COUNT;
        sample_squared_means[i] /= SAMPLE_COUNT;
        const double std_dev{std::sqrt(sample_squared_means[i] - std::pow(sample_means[i], 2))};
        std::cout << std::fixed << "Miner " << miners[i].id << " with " << miners[i].perc << "% of the hashrate: ";
        std::cout << std::fixed << "sample mean " << sample_means[i] * 100 << " std dev of sample mean " << std_dev * 100 << std::endl;
    }
}

// Analyze a sample of the distribution of interval between blocks. We expect the mean to be 600'000 ms on average and
// the standard deviation to be the same as this is an exponential distribution.
void BlockIntervalSample()
{
    std::random_device rd;
    RNG rng{rd()};
    constexpr int SAMPLE_SIZE{100'000'000};

    double mean{0.0}, squared_mean{0.0};
    for (int i{0}; i < SAMPLE_SIZE; ++i) {
        const auto interval{static_cast<double>(NextBlockInterval(rng).count())};
        mean += interval;
        squared_mean += std::pow(interval, 2);
    }
    mean /= SAMPLE_SIZE;
    squared_mean /= SAMPLE_SIZE;

    const double variance{squared_mean - std::pow(mean, 2)};
    std::cout << std::fixed << "Mean " << mean << " std dev " << std::sqrt(variance) << std::endl;
}

int main()
{
    //MinerPickerSample();
    //BlockIntervalSample();
    //MinerPickerSmallBig();
    SimpleSim();
}
