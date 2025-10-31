import math

LAMBDA = 1 / 600

# 
MAJOR_POOLS = [("FOUNDRY", 0.29), ("ANTPOOL", 0.3), ("VIABTC", 0.12), ("F2POOL", 0.11), ("SPIDER", 0.08), ("MARA", 0.05), ("SECPOOL", 0.03)]  # 97%

POOLS = MAJOR_POOLS + [("SMALL", 0.012), ("VERYSMALL", 0.006), ("TINY", 0.002)]

# The propagation times (in seconds) to get the stale rate of each pool for.
prop_times = list(range(30 + 1))

# Mapping from pool name to a list of stale rates at various propagation time values.
stale_rates = {}

def exp_dist_cdf(lambda_param, duration):
    """Exponential distribution CDF for given duration."""
    return 1 - math.exp(-lambda_param * duration)

def p_finds_before(prop_sec, h):
    """Probability that a miner with share {h} of network hashrate finds a block
    within {prop_sec} seconds."""
    this_lambda = LAMBDA * h
    return exp_dist_cdf(this_lambda, prop_sec)

for (name, h) in POOLS:
    stale_rates[name] = []
    for prop_sec in prop_times:
        other_pools = [p for p in POOLS if p != (name, h)]

        # Probability that another miner finds a block before they receive ours,
        # and then mines another on top before we do.
        p_after = 0
        for (_, o_h) in other_pools:
            p_after += p_finds_before(prop_sec, o_h) * o_h

        # Probability they found a block within the T seconds before we found ours,
        # and that another block is found on top (by the entire rest of the network
        # since their block would have reached them before ours did -- although when
        # their block does reach all the other miners we need to take into account the
        # probability that they had found one themselves before).
        p_before = 0
        for (_, o_h) in other_pools:
            # Lambda parameter for this specific miner
            this_lambda = LAMBDA * o_h
            # This miner happens to have found a block in the T seconds that preceded
            # our announcement.
            p_found = (1 - math.exp(-this_lambda * prop_sec))
            # The probability that all the other miners start mining on this block is
            # that they didn't find one in the T seconds that preceded this other miner's
            # block announcement.
            h_rest = 1 - h - o_h
            lambda_rest = LAMBDA * h_rest
            p_other_mine = (1 - math.exp(-lambda_rest * prop_sec))
            # The probability that this miner found a block before us AND another block
            # is found on top of that.
            p_before += p_found * (o_h + h_rest * p_other_mine)

        # An alternative to compute p_before that more accurately compute the hashrate
        # mining on the competing block
        p_bef = 0
        for (o_name, o_h) in other_pools:
            # Lambda parameter for this specific miner
            this_lambda = LAMBDA * o_h
            # This miner happens to have found a block in the T seconds that preceded
            # our announcement.
            p_found = (1 - math.exp(-this_lambda * prop_sec))
            # The probability that all the other miners start mining on this block is
            # that they didn't find one in the T seconds that preceded this other miner's
            # block announcement.
            h_rest = 0
            rest_pools = [p for p in other_pools if p != (o_name, o_h)]
            h_rest = sum((1 - p_finds_before(prop_sec, r_h)) * r_h for (_, r_h) in rest_pools)
            #print(h, o_h, h_rest)
            # The probability that this miner found a block before us AND another block
            # is found on top of that.
            p_bef += p_found * (o_h + h_rest)

        p_other_race = sum(p_finds_before(prop_sec, o_h) for (_, o_h) in other_pools)
        p_before_new = (1 - h) * p_other_race

        p_single_race = sum(p_finds_before(prop_sec * 2, o_h) * o_h for (_, o_h) in other_pools)

        print(prop_sec, h, p_single_race, p_after, p_before, p_before_new, p_bef)

        # FIXME: decide
        p_before = p_before_new
        p_before = p_bef

        stale_rates[name].append((p_before + p_after) * 100)  # %

        # TODO: should take into account diff adjustment



import matplotlib.pyplot as plt

fig, ax = plt.subplots()
for name in stale_rates:
    ax.plot(prop_times, stale_rates[name], label=name)
ax.set_xlabel("Propagation time (seconds)")
ax.set_ylabel("Stale rate (%)")
ax.legend()
plt.show()

