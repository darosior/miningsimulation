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

void PrintChain(const Miner& miner)
{
    std::cout << "Miner " << miner.id << " chain: ";
    for (const auto&b : miner.chain) {
        std::cout << "(" << b.miner_id << ", " << b.arrival << "), ";
    }
    std::cout << std::endl;
}

/** Test our implementation of the "worst case" (gamma=0) selfish mining strategy. This goes over all the possible state in
 * model presented in section 4.2 of the 2013 paper. We also exercise some scenarii not present in the 2013 paper's model.
 */
void TestSelfishStrategy()
{
    constexpr int SM_ID{0}, OTHERS_ID{1};
    constexpr std::chrono::milliseconds SM_PROP_TIME{100ms};
    Miner selfish_miner{SM_ID, 35, SM_PROP_TIME, true};

    /** Case (a), any state but two branches of length 1, pool finds a block. */
    // Start with a public chain of 2 blocks (+ genesis)
    selfish_miner.chain.emplace_back(OTHERS_ID, 600s);
    selfish_miner.chain.emplace_back(SM_ID, 600s * 2);

    // Private fork of 0 block, best chain fork of 0 block, pool finds a block. "The pool appends
    // one block to its private branch, increasing its lead on the public branch by one."
    selfish_miner.FoundBlock(600s * 3, selfish_miner.chain.size());
    assert(selfish_miner.chain.size() == 4);
    assert(selfish_miner.chain[3].miner_id == SM_ID && selfish_miner.chain[3].arrival == SELFISH_ARRIVAL);

    // Private chain of 1 block, best chain fork of 0 block, pool finds a block. "The pool appends
    // one block to its private branch, increasing its lead on the public branch by one."
    selfish_miner.FoundBlock(600s * 4, 3);
    assert(selfish_miner.chain.size() == 5);
    std::vector<Block> expected_chain{Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)};
    assert(selfish_miner.chain == expected_chain);

    /** Case (b), was two branches of length 1, pool finds a block. */
    // Set the chain of the selfish miner accordingly to a 4 blocks best chain and its 1-block fork on top.
    selfish_miner.chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, SELFISH_ARRIVAL)};

    // Now the selfish miner finds a block. "The pool publishes its secret branch of length two".
    selfish_miner.FoundBlock(600s * 6, 5); // best chain size is 5 cause the rest of the miners have a 1-block fork too.
    expected_chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3),
        Block(SM_ID, 600s*6 + SM_PROP_TIME), Block(SM_ID, 600s*6 + SM_PROP_TIME)
    };
    assert(selfish_miner.chain == expected_chain);

    /** Case (c), was two branches of length 1, others find a block after pool head. */
    // This never happens in our simulation since we only implement gamma=0

    /** Case (d), was two branches of length 1, others find a block after others’ head. */
    // Set the chain of the selfish miner accordingly to a 4 blocks best chain and its 1-block fork on top.
    selfish_miner.chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, SELFISH_ARRIVAL)};

    // Now the selfish miner is notified of a longer best chain with the last two blocks being the others'. He
    // switches to mining on top of it.
    std::vector<Block> best_chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(OTHERS_ID, 600s*4), Block(OTHERS_ID, 600s*5)};
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*5);
    assert(selfish_miner.chain == best_chain);

    /** Case (e), no private branch, others find a block. */
    // Set the chain of the selfish miner accordingly to a 5 blocks best chain with no private fork on top.
    selfish_miner.chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, 600s*4)};

    // Now the selfish miner is notified of a longer best chain with the last block being the other's. He
    // switches to mining on top of it.
    best_chain = selfish_miner.chain;
    best_chain.emplace_back(OTHERS_ID, 600s*5);
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*5);
    assert(selfish_miner.chain == best_chain);

    /** Case (f), lead was 1, others find a block. "Now there are two branches of length one, and the pool
     * publishes its single secret block." */
    // Set the chain of the selfish miner accordingly to a 3 blocks best chain with a 1-block private fork on top.
    selfish_miner.chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(SM_ID, SELFISH_ARRIVAL)};

    // Now the selfish miner is notified of an equal-size best chain with the last block being the others'. He reveals
    // his last block and continues mining on top of it.
    best_chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3)};
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*3);
    expected_chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(SM_ID, 600s*3 + SM_PROP_TIME)};
    assert(selfish_miner.chain == expected_chain);

    /** Case (g), lead was 2, others find a block. "The others almost close the gap as the lead drops to 1.
     * The pool publishes its secret blocks, causing everybody to start mining at the head of the previously
     * private branch, since it is longer". */
    // Set the chain of the selfish miner accordingly to a 3 blocks best chain with a 2-blocks private fork on top.
    selfish_miner.chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)};

    // Now the selfish miner is notified of a best public chain with only one block less than his private chain.
    // He reveals all his private blocks.
    best_chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3)};
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*3);
    expected_chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(SM_ID, 600s*3 + SM_PROP_TIME), Block(SM_ID, 600s*3 + SM_PROP_TIME)};
    assert(selfish_miner.chain == expected_chain);

    /** Case (h), lead was more than 2, others win. The others decrease the lead, which remains at least two.
     * The new block (say with number i) will end outside the chain once the pool publishes its entire branch. */
    // Set the chain of the selfish miner accordingly to a 3 blocks best chain with a 3-block private fork on top.
    selfish_miner.chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(SM_ID, SELFISH_ARRIVAL),
        Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)
    };

    // Now the selfish miner is notified of a best public chain with two blocks less than his private chain. He reveals
    // the oldest block and keeps mining on its private fork.
    best_chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3)};
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*3);
    expected_chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(SM_ID, 600s*3 + SM_PROP_TIME),
        Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)
    };
    assert(selfish_miner.chain == expected_chain);

    // Set the chain of the selfish miner accordingly to a 4 blocks best chain with a 5-block private fork on top.
    selfish_miner.chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, SELFISH_ARRIVAL),
        Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)
    };

    // Now the selfish miner is notified of a best public chain with four blocks less than his private chain. He reveals
    // the oldest block and keeps mining on its private fork.
    best_chain = {Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(OTHERS_ID, 600s*4)};
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*4);
    expected_chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, 600s*4 + SM_PROP_TIME),
        Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)
    };
    assert(selfish_miner.chain == expected_chain);

    /** Case absent from the paper. Same as above but the rest of the network found two blocks in a row. */
    // Set the chain of the selfish miner accordingly to a 4 blocks best chain with a 5-block private fork on top.
    selfish_miner.chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, SELFISH_ARRIVAL),
        Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)
    };

    // Now the selfish miner is notified of a best public chain with four blocks less than his private chain. He reveals
    // the oldest block and keeps mining on its private fork.
    best_chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3),
        Block(OTHERS_ID, 600s*4), Block(OTHERS_ID, 600s*5)
    };
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*5);
    expected_chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, 600s*5 + SM_PROP_TIME),
        Block(SM_ID, 600s*5 + SM_PROP_TIME), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL), Block(SM_ID, SELFISH_ARRIVAL)
    };
    assert(selfish_miner.chain == expected_chain);

    /** Case absent from the paper. Selfish miner has a 1-block lead and other miners find two blocks in a row. */
    // Set the chain of the selfish miner accordingly to a 4 blocks best chain with a 1-block private fork on top.
    selfish_miner.chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3), Block(SM_ID, SELFISH_ARRIVAL),
    };

    // Now the selfish miner is notified of a best public chain with 1 block more than his private one. He switches to it.
    best_chain = {
        Block::Genesis(), Block(OTHERS_ID, 600s), Block(SM_ID, 600s*2), Block(OTHERS_ID, 600s*3),
        Block(OTHERS_ID, 600s*4), Block(OTHERS_ID, 600s*5)
    };
    selfish_miner.NotifyBestChain({best_chain.begin(), best_chain.end()}, 600s*5);
    assert(selfish_miner.chain == best_chain);

    std::cout << "Selfish mining strategy tests passed." << std::endl;
}

int main()
{
    //MinerPickerSample();
    //BlockIntervalSample();
    //MinerPickerSmallBig();
    //SimpleSim();
    TestSelfishStrategy();
}
