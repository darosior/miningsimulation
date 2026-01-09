import math
import matplotlib.pyplot as plt

# Rate of arrival of blocks in seconds for the exponential distribution
LAMBDA = 1 / 600

# Hashrate distribution. Covers 97%.
MAJOR_POOLS = {"ANTPOOL": 0.3, "FOUNDRY": 0.29, "VIABTC": 0.12, "F2POOL": 0.11, "SPIDER": 0.08, "MARA": 0.05, "SECPOOL": 0.03}

# The remaining 3% are some invented smaller pools.
SMALL_POOLS = {"SMALL": 0.012, "VERYSMALL": 0.006, "TINY": 0.002}

# The entire distribution of network hashrate.
POOLS = {}
POOLS.update(MAJOR_POOLS)
POOLS.update(SMALL_POOLS)

def exp_dist_cdf(lambda_param, duration):
    """Exponential distribution CDF for given duration."""
    return 1 - math.exp(-lambda_param * duration)

def p_finds_within(prop_sec, h):
    """Probability that a miner with share {h} of network hashrate finds a block
    within {prop_sec} seconds."""
    this_lambda = LAMBDA * h
    return exp_dist_cdf(this_lambda, prop_sec)

def p_stale_before(prop_sec, h):
    """Probability that a block found by a miner with share {h} of network hashrate
    goes stale due to another miner having found a block under {prop_sec} before."""
    p_race = p_finds_within(prop_sec, 1 - h)
    p_finds_next = 1 - h
    return p_race * p_finds_next

def p_stale_after(prop_sec, net_hs):
    """Probability that any miner from the list {net_hs} of network hashrates finds
    a block within {prop_sec} and then finds another block on top."""
    return sum(p_finds_within(prop_sec, h) * h for h in net_hs)

def get_stale_rates(prop_times):
    """Get a mapping from pool name to a list of stale rates at various propagation times."""
    stale_rates = {}

    for (name, hashrate) in POOLS.items():
        stale_rates[name] = []
        for prop_sec in prop_times:
            # The stale rate for this miner depends on the probability of its block going
            # stale due to another miner finding a block before it did, but it hadn't heard
            # about it yet, plus the probability of it going stale because any other miner
            # found a competing block before it reached them.
            p_before = p_stale_before(prop_sec, hashrate)
            network_hashrates = [h for (o_name, h) in POOLS.items() if (o_name, h) != (name, hashrate)]
            p_after = p_stale_after(prop_sec, network_hashrates)
            stale_rates[name].append(p_before + p_after)

    return stale_rates

def get_net_benefits(prop_times):
    """Get a mapping from pool name to a list of advantages (in relative revenue increase)
    at various propagation times."""
    stale_rates = get_stale_rates(prop_times)

    benefits = {}
    for i in range(len(prop_times)):
        # Because the difficulty readjusts, we are interested in the proportion of blocks
        # found by this miner *among* all the blocks actually accepted by the network (ie,
        # discounting the stale blocks).
        total_stale = sum(POOLS[name] * r[i] for name, r in stale_rates.items())
        total_found = 1 - total_stale
        for name in stale_rates:
            if name not in benefits:
                benefits[name] = []
            pool_hashrate = POOLS[name]
            actual_share = pool_hashrate * (1 - stale_rates[name][i]) / total_found
            benefits[name].append((actual_share - pool_hashrate) / pool_hashrate)

    return benefits

def plot_stale_rates(prop_times):
    stale_rates = get_stale_rates(prop_times)

    fig, ax = plt.subplots()
    for name in stale_rates:
        rates_perc = [rate * 100 for rate in stale_rates[name]]
        ax.plot(prop_times, rates_perc, label=name)
    ax.set_xlabel("Propagation time (seconds)")
    ax.set_ylabel("Stale rate (%)")
    ax.legend(reverse=True)

    plt.show()

def plot_benefits(prop_times):
    benefits = get_net_benefits(prop_times)

    fig, ax = plt.subplots()
    for name in benefits:
        rates_perc = [rate * 100 for rate in benefits[name]]
        ax.plot(prop_times, rates_perc, label=name)
    ax.set_xlabel("Propagation time (seconds)")
    ax.set_ylabel("Change in revenue (%)")
    ax.legend()

    plt.show()

if __name__ == "__main__":
    # The propagation times (in seconds) to get the stale rate of each pool for.
    prop_times = list(range(20 + 1))

    plot_stale_rates(prop_times)
    plot_benefits(prop_times)
