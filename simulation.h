#include <algorithm>
#include <cassert>
#include <chrono>
#include <optional>
#include <limits>
#include <random>
#include <ranges>
#include <vector>

#include "xoroshiro128++.h"

using namespace std::chrono_literals;

//! Expected time between blocks. Used as parameter for the exponential distribution we are sampling from.
static constexpr std::chrono::seconds BLOCK_INTERVAL{600};
//! We use integers in [0;100] for percentages. This is the multiplier to map them to [0; uint64_t::MAX].
static constexpr uint64_t PERC_MULTIPLIER{std::numeric_limits<uint64_t>::max() / 100};
//! A block is identified by its position among all the blocks found during a simulation, see `BlockTree`.
using BlockId = size_t;
//! The genesis block, not created by any miner and which all miners start mining on top of.
static constexpr BlockId GENESIS{0};

struct Block {
    //! Which miner created this block.
    unsigned miner_id;
    //! The block this one was found on top of.
    BlockId parent;
    //! How many blocks there are before this one in its chain.
    size_t height;
};

/** All the blocks found during a simulation. Each block records the one it was found on top of: a chain is
 * identified by its last block, and two competing chains are two branches of this tree. */
struct BlockTree {
    std::vector<Block> blocks;

    explicit BlockTree(): blocks{Block{std::numeric_limits<unsigned>::max(), GENESIS, 0}} {}

    /** Record a block found by this miner on top of the given block. */
    BlockId Add(unsigned miner_id, BlockId parent) {
        blocks.push_back(Block{miner_id, parent, Height(parent) + 1});
        return blocks.size() - 1;
    }

    unsigned MinerId(BlockId block) const { return blocks[block].miner_id; }

    BlockId Parent(BlockId block) const { return blocks[block].parent; }

    size_t Height(BlockId block) const { return blocks[block].height; }

    /** The block at the given height in the chain which ends in `tip`. */
    BlockId Ancestor(BlockId tip, size_t height) const {
        assert(height <= Height(tip));
        while (Height(tip) > height) {
            tip = Parent(tip);
        }
        return tip;
    }

    /** How many blocks of the chain which ends in `tip` were found by this miner. */
    long BlocksFoundBy(unsigned miner_id, BlockId tip) const {
        long count{0};
        for (; tip != GENESIS; tip = Parent(tip)) {
            if (MinerId(tip) == miner_id) ++count;
        }
        return count;
    }
};

/** The strategy followed by a miner. */
enum class Strategy {
    //! Publish blocks as soon as they are found, and mine on top of the longest chain. In the event of a race,
    //! always mine on top of its own block.
    OWN_BLOCK_FIRST,
    //! The (worst case) selfish mining strategy as described in section 3.2 of https://arxiv.org/pdf/1311.0243.
    SELFISH,
};

struct Miner {
    //! Miner identifier used to track which miner created a certain block.
    unsigned id;
    //! Share of the total network hashrate controlled by the miner, as an integer between 0 and 100.
    uint64_t perc;
    //! The time for blocks published by this miner to reach all other miners.
    std::chrono::milliseconds propagation;
    //! The strategy followed by this miner.
    Strategy strategy;
    //! The time for a block found by the hashers of this miner to reach the miner's node.
    std::chrono::milliseconds submit_delay;
    //! The time for the hashers of this miner to start working on top of a block, once the miner's node switched
    //! to it. This goes for a block received from another miner as well as for a block they found themselves.
    std::chrono::milliseconds work_delay;
    //! The last block of the chain of this miner's node. May differ slightly between miners due to propagation
    //! time. For a selfish miner the last blocks of this chain may not be published.
    BlockId tip{GENESIS};
    //! The block the hashers of this miner are working on top of. This is the last block of the miner's chain,
    //! or an earlier one if they have not received work on top of it yet.
    BlockId work_tip{GENESIS};
    //! The last block of the best published chain this miner knows of. A selfish miner compares its own
    //! chain to it.
    BlockId public_tip{GENESIS};
    //! How many blocks at the end of its chain a selfish miner has not published yet. NOTE: this is not
    //! `privateBranchLen` from the paper's algorithm, which also counts the blocks of the private branch
    //! that were already published.
    size_t withheld_blocks{0};
    //! How many blocks this miner found.
    long blocks_found{0};

    explicit Miner(unsigned id_, uint64_t perc_, std::chrono::milliseconds prop, Strategy strategy_ = Strategy::OWN_BLOCK_FIRST,
                   std::chrono::milliseconds submit_delay_ = 0ms, std::chrono::milliseconds work_delay_ = 0ms)
        : id{id_}, perc{perc_}, propagation{prop}, strategy{strategy_}, submit_delay{submit_delay_}, work_delay{work_delay_}
    {}

    /** The node of this miner receives a block found by the miner's hashers. Returns this block if the miner
     * publishes it. */
    std::optional<BlockId> FoundBlock(const BlockTree& blocks, BlockId block) {
        blocks_found++;

        if (blocks.Height(block) > blocks.Height(tip)) {
            // The block makes our chain longer. A selfish miner is in a race if its chain was the same size
            // as the best published chain but ended in a different block.
            const bool in_race{blocks.Height(tip) == blocks.Height(public_tip) && tip != public_tip};
            tip = block;
            if (strategy == Strategy::SELFISH && !in_race) {
                // A selfish miner always withholds the blocks it finds, except in the case of a race.
                withheld_blocks++;
                return std::nullopt;
            }
            // A selfish miner which finds a block while in a race publishes it immediately, as its chain
            // is now the longest. The blocks before it were already published by MaybeSelfishReveal().
            return tip;
        }

        // Our hashers were not working on top of the last block of our chain when they found this one, as
        // they had not received work on top of it yet. The block does not make our chain longer. If it makes
        // it the same size as a chain ending in another miner's block we are in a race: switch to our own
        // block and publish it.
        const bool race_with_other{blocks.Height(block) == blocks.Height(tip) && blocks.MinerId(tip) != id};
        if (race_with_other) {
            tip = block;
            withheld_blocks = 0;
            return tip;
        }

        // Otherwise the block is stale: either our chain is already longer, or it ends in another block of
        // ours which we keep as other miners may already be working on top of it.
        return std::nullopt;
    }

    /** If this miner follows the selfish mining strategy, choose whether to selectively reveal some
     * blocks. The strategy implemented here follows the one described in the 2013 "Majority is not enough"
     * research paper in the worst case scenario, ie Gamma=0 (in the case of a 1-block race no other miner
     * mines on top of a selfish miner's block). Paper available at https://arxiv.org/pdf/1311.0243.
     *
     * Returns the last of the blocks it reveals, if any. All the blocks before it are published along with it.
     */
    std::optional<BlockId> MaybeSelfishReveal(const BlockTree& blocks) {
        if (strategy != Strategy::SELFISH) return std::nullopt;

        // Our chain is at least the same size as the best published one, we keep mining on it. Note that even
        // when they are the same size, we may be mining on top of a different block still in the case of a
        // 1-block race.
        // If they are catching up, reveal as many blocks as they have just found.
        const size_t current_lead{blocks.Height(tip) - blocks.Height(public_tip)};
        if (withheld_blocks <= current_lead) return std::nullopt;
        size_t reveal_count{withheld_blocks - current_lead};
        // Special case: if we had a significant lead and they are almost caught up reveal everything
        // now to avoid a race.
        if (withheld_blocks > 1 && current_lead == 1) {
            reveal_count = withheld_blocks;
        }
        withheld_blocks -= reveal_count;
        return blocks.Ancestor(tip, blocks.Height(tip) - withheld_blocks);
    }

    /** Let this miner know a block was received by all the miners which did not find it, and with it all the
     * blocks before it in its chain. The miner which found this block is told too: its block is now part of
     * a published chain. Returns a block this miner publishes in reaction, if any. */
    std::optional<BlockId> ReceivedBlock(const BlockTree& blocks, BlockId block) {
        // Among published chains of the same size, the best one is the first one received (matching Bitcoin
        // Core's first-seen rule).
        if (blocks.Height(block) > blocks.Height(public_tip)) {
            public_tip = block;
        }

        // Of course we assume all blocks are at the same difficulty. If the best published chain is longer
        // than ours we have to switch to it, even if we had not published the last blocks of ours.
        if (blocks.Height(public_tip) > blocks.Height(tip)) {
            tip = public_tip;
            withheld_blocks = 0;
            return std::nullopt;
        }

        return MaybeSelfishReveal(blocks);
    }

    /** How many of the blocks this miner found are not in its chain anymore. NOTE: a block which was dropped
     * from its chain is not stale anymore if it makes it back into it, which happens when another miner kept
     * mining on top of it. */
    long StaleBlocks(const BlockTree& blocks) const {
        return blocks_found - blocks.BlocksFoundBy(id, tip);
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

/** Statistics about a miner's revenue in function of the best chain. */
struct MinerStats {
    //! The count of blocks found by this miner in the best chain.
    long blocks_found;
    //! The ratio of blocks found by this miner in the best chain.
    double blocks_share;
    //! The ratio of stale blocks over all the blocks found by this miner, whether in the best chain or not.
    double stale_rate;

    /** Compute revenue statistics for this miner in the best chain, which ends in `best_tip`. */
    explicit MinerStats(const Miner& miner, const BlockTree& blocks, BlockId best_tip)
    {
        blocks_found = blocks.BlocksFoundBy(miner.id, best_tip);
        // The height of the last block is the number of blocks in the chain, not counting the genesis block.
        blocks_share = blocks_found == 0 ? 0.0 : static_cast<double>(blocks_found) / blocks.Height(best_tip);
        // The blocks found by this miner are those which made it to the best chain plus the stale ones.
        const long stale_blocks{miner.StaleBlocks(blocks)};
        const long total_found{blocks_found + stale_blocks};
        stale_rate = total_found == 0 ? 0.0 : static_cast<double>(stale_blocks) / total_found;
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

/** Something which happens at a given time during a simulation. */
struct Event {
    enum class Type {
        //! The hashers of one of the miners find a block.
        BLOCK_FOUND,
        //! A block found by the hashers of a miner is received by the node of this miner.
        BLOCK_SUBMITTED,
        //! A block published by a miner is received by all the other miners.
        BLOCK_RECEIVED,
        //! The hashers of a miner start working on top of a block.
        WORK_RECEIVED,
    };

    std::chrono::milliseconds time;
    Type type;
    //! The block concerned. Not used when a block is found: which miner finds it is only drawn then.
    BlockId block{GENESIS};
    //! The miner whose hashers receive work. Not used by the other events.
    unsigned miner_id{0};
};

/** Simulate the Bitcoin mining process for a given amount of time with the given miners, each having its
 * own share of network hashrate and block propagation time.
 *
 * The propagation time is a simplification: it is the time before which a miner's block has not reached
 * any other miner and after which it has reached all other miners.
 *
 * A miner is made of a node and of hashers. By default they act as one: a block found by its hashers is published
 * at once, and they start working on top of a block as soon as it is received. Each miner can instead be given a
 * delay for each direction, see `Miner::submit_delay` and `Miner::work_delay`.
 *
 * The mining process is accurately modeled: we draw the time between the last and next block from an
 * exponential distribution, then draw which miner found this block based on its hashrate and a uniform
 * distribution. Difficulty and network hashrate are assumed to be constant.
 *
 * This assumes today's miners' behaviour by default: a miner will mine on top of its own block immediately and will
 * only switch to a propagated chain if it's longer (again, difficulty is assumed constant). Miners can optionally
 * be set to adopt another strategy in SetupMiners(), see `Strategy`.
 *
 * All the randomness is derived from the given seed: the same seed always results in the same simulation.
 */
std::vector<MinerStats> RunSimulation(std::chrono::milliseconds duration_time, std::vector<Miner> miners, uint64_t seed)
{
    // Random number generators used to respectively pick the time before the next block interval and
    // pick which miner found the last block.
    RNG seeder{seed};
    RNG block_interval{seeder.rand64()}, miner_picker{seeder.rand64()};

    // Miners are looked up by identifier.
    for (size_t i{0}; i < miners.size(); ++i) {
        assert(miners[i].id == i);
    }

    BlockTree blocks;
    // The last block of the best published chain. Among chains of the same size, this is the one which was
    // received first (matching Bitcoin Core's first-seen rule).
    BlockId best_tip{GENESIS};

    // Only the events of `Event::Type` may change the state of miners (including the selfish ones). We keep
    // track of those to come, starting with the first block to be found. Since we are starting from 0, its
    // time is just the block interval itself.
    std::vector<Event> events;
    events.push_back({NextBlockInterval(block_interval), Event::Type::BLOCK_FOUND});

    // The node of a miner receives a block, either from the miner's own hashers or from another miner. Record
    // what follows from it: the other miners receive the block the miner publishes in reaction, if any, and
    // if its node switched to another block the miner's hashers receive work on top of it.
    const auto node_receives_block = [&](Miner& miner, BlockId block, bool from_hashers, std::chrono::milliseconds time) {
        const BlockId previous_tip{miner.tip};
        const auto published{from_hashers ? miner.FoundBlock(blocks, block) : miner.ReceivedBlock(blocks, block)};
        if (published) {
            events.push_back({time + miner.propagation, Event::Type::BLOCK_RECEIVED, *published});
        }
        if (miner.tip != previous_tip) {
            events.push_back({time + miner.work_delay, Event::Type::WORK_RECEIVED, miner.tip, miner.id});
        }
    };

    // Run the simulation. Instead of iterating through every ms where nothing will happen, go from one event
    // to the next. Events which happen at the same time are handled in the order they were added, except that
    // a block is found last: by then the miners know about everything else which happens at that time.
    while (true) {
        // NOTE: there is always an event to come, since finding a block adds the time of the next one.
        const auto next_event{std::ranges::min_element(events, {}, [](const Event& event) {
            return std::pair{event.time, event.type == Event::Type::BLOCK_FOUND};
        })};
        const Event event{*next_event};
        if (event.time >= duration_time) break;
        events.erase(next_event);

        switch (event.type) {
        case Event::Type::BLOCK_FOUND: {
            // A block was found, draw which miner found it. Its hashers found it on top of the block they
            // were working on, and send it to the miner's node.
            const Miner& miner{PickFinder(miners, miner_picker)};
            const BlockId block{blocks.Add(miner.id, miner.work_tip)};
            events.push_back({event.time + miner.submit_delay, Event::Type::BLOCK_SUBMITTED, block});
            events.push_back({event.time + NextBlockInterval(block_interval), Event::Type::BLOCK_FOUND});
            break;
        }
        case Event::Type::BLOCK_SUBMITTED: {
            // The node of the miner which found this block receives it. It may not publish it right away, or
            // not at all.
            node_receives_block(miners[blocks.MinerId(event.block)], event.block, /* from_hashers = */ true, event.time);
            break;
        }
        case Event::Type::BLOCK_RECEIVED: {
            // A block was received, let all miners know about it. They might switch to it if its chain is
            // longer than theirs, or act upon the information (for instance a selfish miner may selectively
            // reveal some of its private blocks).
            if (blocks.Height(event.block) > blocks.Height(best_tip)) {
                best_tip = event.block;
            }
            for (auto& miner: miners) {
                node_receives_block(miner, event.block, /* from_hashers = */ false, event.time);
            }
            break;
        }
        case Event::Type::WORK_RECEIVED: {
            miners[event.miner_id].work_tip = event.block;
            break;
        }
        }
    }

    std::vector<MinerStats> stats;
    for (const auto& miner: miners) {
        stats.emplace_back(MinerStats(miner, blocks, best_tip));
    }

    return stats;
}

/** Utility function useful in tests or for debugging. Prints which miner found each block of this miner's chain. */
void PrintChain(const Miner& miner, const BlockTree& blocks)
{
    std::vector<unsigned> miner_ids;
    for (BlockId block{miner.tip}; block != GENESIS; block = blocks.Parent(block)) {
        miner_ids.push_back(blocks.MinerId(block));
    }
    std::cout << "Miner " << miner.id << " chain: ";
    for (const auto miner_id: std::views::reverse(miner_ids)) {
        std::cout << miner_id << ", ";
    }
    std::cout << std::endl;
}
