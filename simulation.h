#include <algorithm>
#include <cassert>
#include <chrono>
#include <optional>
#include <limits>
#include <memory>
#include <random>
#include <ranges>
#include <span>

#include "xoroshiro128++.h"

using namespace std::chrono_literals;

//! Expected time between blocks. Used as parameter for the exponential distribution we are sampling from.
static constexpr std::chrono::seconds BLOCK_INTERVAL{600};
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

    bool operator==(const Block& other) const {
        // FIXME: should we have a block uid for when a single miner finds two blocks in a row with the same timestamp?
        return miner_id == other.miner_id && arrival == other.arrival;
    }
};

/** The size of a chain and its last block. */
struct ChainTip {
    size_t size;
    Block block;
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
    //! The tip of the best published chain this miner was last notified of. A selfish miner needs it to
    //! tell whether it is in a race when it finds a block.
    ChainTip best_tip;

    explicit Miner(unsigned id_, uint64_t perc_, std::chrono::milliseconds prop, bool selfish = false)
        : id{id_}, perc{perc_}, propagation{prop}, chain{{Block::Genesis()}}, stale_blocks{0}, is_selfish{selfish}, best_tip{1, Block::Genesis()}
    {}

    /** Add a block found at the given block time to this miner's local chain. */
    void FoundBlock(std::chrono::milliseconds block_time) {
        // A selfish miner is in a race if its chain is the same size as the best published chain but ends
        // in a different block.
        const bool in_race{chain.size() == best_tip.size && chain.back() != best_tip.block};
        if (is_selfish && !in_race) {
            // A selfish miner always withholds the blocks it finds, except in the case of a race.
            chain.emplace_back(id, SELFISH_ARRIVAL);
        } else {
            // A selfish miner which finds a block while in a race publishes it immediately, as its chain
            // is now the longest. The blocks before it were already revealed by MaybeSelfishReveal().
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

    /** The earliest block published by this miner that has not yet propagated to the network, if any. */
    std::optional<std::chrono::milliseconds> NextArrival(std::chrono::milliseconds cur_time) const {
        std::optional<std::chrono::milliseconds> earliest_arrival{};
        for (const auto& block: std::views::reverse(chain)) {
            // Arrival time is monotonic.
            if (block.arrival <= cur_time) {
                break;
            }
            earliest_arrival = block.arrival;
        }
        return earliest_arrival;
    }

    /** Number of blocks a selfish miner has not revealed yet. NOTE: this is not `privateBranchLen` from the
     * paper's algorithm, which also counts the blocks of the private branch that were already revealed. */
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

        // Find the point of agreement between the two chains.
        for (size_t i{chain.size()}; i > 0; --i) {
            if (chain.back() == best_chain[i - 1]) {
                break;
            }
            if (chain.back().miner_id == id) stale_blocks++;
            chain.pop_back();
        }

        // Adopt the best chain. NOTE: it may contain a block of ours we had dropped for another chain,
        // and counted as stale, if another miner kept mining on top of it.
        assert(best_chain.size() > chain.size()); // Checked above.
        for (size_t i{chain.size()}; i < best_chain.size(); ++i) {
            if (best_chain[i].miner_id == id) stale_blocks--;
            chain.push_back(best_chain[i]);
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
            // Broadcast as many blocks as necessary by setting their arrival time. We reveal them in reaction
            // to a block the other miners received at the same time as us, so ours necessarily arrive after
            // it even if they propagate instantly. NOTE: this also makes sure the simulation never misses
            // a block revealed now, as it only looks for blocks arriving after the current time.
            const auto arrival{cur_time + std::max(propagation, 1ms)};
            for (size_t i{0}; i < reveal_count; ++i) {
                chain.at(chain.size() - selfish_count + i).arrival = arrival;
            }
        }
    }

    /** Let this miner know about the longest published chain. */
    void NotifyBestChain(std::span<const Block> best_chain, std::chrono::milliseconds cur_time) {
        MaybeSelfishReveal(best_chain, cur_time);
        MaybeReorg(best_chain);
        // Remember where the best chain stands, FoundBlock() needs to know.
        best_tip = {best_chain.size(), best_chain.back()};
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

    /** Proportion of stale blocks among all the blocks found by this miner. */
    double StaleRate(std::chrono::milliseconds cur_time) const {
        // BlocksFound() only counts the blocks still in our chain, i.e. those which did not go stale.
        long total_found{BlocksFound(cur_time) + stale_blocks};
        if (total_found == 0) return 0.0;
        return static_cast<double>(stale_blocks) / total_found;
    }
};

/** Draw the time between the last and the next block from the given exponential distribution. */
std::chrono::milliseconds NextBlockInterval(RNG& rng)
{
    const auto exporand{std::llround(rng.exporand(std::chrono::nanoseconds(BLOCK_INTERVAL).count()))};
    assert(exporand >= 0.0); // Must not go backward.
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(exporand));
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

/** Utility function useful in tests or for debugging. */
void PrintChain(const Miner& miner)
{
    std::cout << "Miner " << miner.id << " chain: ";
    for (const auto&b : miner.chain) {
        std::cout << "(" << b.miner_id << ", " << b.arrival << "), ";
    }
    std::cout << std::endl;
}
