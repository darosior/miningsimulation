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
        miners.back().chain.reserve((TOTAL_BLOCK_COUNT + 99) / 100);
    }
    assert(miners.size() == 100);

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
    BlockIntervalSample();
}
