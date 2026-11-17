# Block propagation simulator

A simple block propagation simulation. Create a network with a given number of miners, each with its
own proportion of the network hashrate and block propagation time. The block propagation time is a
simplification: before the threshold no other miner has seen a block, after it all miners have seen
it. The simulation proceeds by advancing one millisecond at a time from the start, responding to
events such as new block found, block previously found propagated to the rest of the network, etc.
The time between blocks is drawn from an exponential distribution with mean 600 seconds. Which miner
found the last block is drawn randomly based on each miner's proportion of the network hashrate.

One goal of this simulation is to be simple yet close to reality. The simplified block propagation
modeling is an example of that. It also implements only the worst-case selfish mining strategy (no
Sybil, i.e. $\gamma = 0$ according to the 2013 paper).

Thanks to Clara Shikhelman for suggesting a "more obviously correct" way of drawing block intervals
and attributions. Thanks to Pieter Wuille for suggesting to use and providing the code for a faster
RNG than the default one from the STL. Thanks to Claude for fixing some of my human slop a year
later.

# Running the simulation

By default the program will run 32768 simulations for a year (in parallel) and print statistics
averaged over all the runs. This can be tweaked by changing the `SIM_DURATION` and `SIM_RUNS`
constants at the top of [`main.cpp`](main.cpp).

The network settings can be tweaked by changing the definition of the `SetupMiners` function in
[`main.cpp`](main.cpp). By default it's been set to approximate the hashrate distribution and block
propagation times at the time of writing.

You will need a C++ compiler compatible with C++20 (any remotely modern C++ compiler will do).
That's it. For instance with clang 19:
```
clang++-19 -O3 -std=c++20 main.cpp -o simulation
```

Then simply run the program:
```
./simulation
```

# Example results

## Impact of block propagation on centralization pressure

Observe how stale blocks inflate the effective hashrate of large miners at the expense of smaller
ones, and how reducing block propagation time helps restore a level playing field.

In this example, the simulation is ran twice with a hashrate distribution similar to that of the
Bitcoin network in 2026. In the first run, block propagation is set to 100 milliseconds. In the
second run it is set to 10 seconds.

It's important to highlight how the increased propagation time ultimately impacts profits, as this
is what drives hashrate towards the largest miners. As an example, consider profit margins of 2% of
revenue, and a block propagation time increase as detailed below.

Once the difficulty has adjusted to the stale blocks, what a miner earns is its share of the blocks
found: a miner with a lower stale rate than average gets a larger share of blocks than its share of
the hashrate. Before the change in propagation time, this increases the profit of the largest miner
by 0.2% and reduces the profit of the smallest miner by 0.3%. After the increase in propagation
time, it **increases the profit of the largest miner by 16%**, and **costs the smallest miner 32%
of its profit**.

Block propagation set to 100ms:
```
Running 32768 simulations in parallel using 32 threads.
100% progress..
After running 32768 simulations for 365d each, on average:
  - Miner 0 (30% of network hashrate) found 15775 blocks i.e. 29.9996% of blocks. Stale rate: 0.010118%.
  - Miner 1 (27% of network hashrate) found 14199 blocks i.e. 27.001% of blocks. Stale rate: 0.0112318%.
  - Miner 2 (13% of network hashrate) found 6836 blocks i.e. 13.0003% of blocks. Stale rate: 0.0160063%.
  - Miner 3 (10% of network hashrate) found 5258 blocks i.e. 9.99942% of blocks. Stale rate: 0.0167833%.
  - Miner 4 (10% of network hashrate) found 5258 blocks i.e. 9.99992% of blocks. Stale rate: 0.0170194%.
  - Miner 5 (5% of network hashrate) found 2629 blocks i.e. 5.00026% of blocks. Stale rate: 0.0185828%.
  - Miner 6 (3% of network hashrate) found 1577 blocks i.e. 2.99978% of blocks. Stale rate: 0.0194372%.
  - Miner 7 (1% of network hashrate) found 525 blocks i.e. 1.00005% of blocks. Stale rate: 0.0199559%.
  - Miner 8 (1% of network hashrate) found 525 blocks i.e. 0.999661% of blocks. Stale rate: 0.0193334%.
```

Block propagation set to 10s:
```
Running 32768 simulations in parallel using 32 threads.
100% progress..
After running 32768 simulations for 365d each, on average:
  - Miner 0 (30% of network hashrate) found 15623 blocks i.e. 30.0974% of blocks. Stale rate: 0.987576%.
  - Miner 1 (27% of network hashrate) found 14046 blocks i.e. 27.0598% of blocks. Stale rate: 1.08593%.
  - Miner 2 (13% of network hashrate) found 6731 blocks i.e. 12.9684% of blocks. Stale rate: 1.54798%.
  - Miner 3 (10% of network hashrate) found 5172 blocks i.e. 9.96531% of blocks. Stale rate: 1.64771%.
  - Miner 4 (10% of network hashrate) found 5172 blocks i.e. 9.96517% of blocks. Stale rate: 1.64856%.
  - Miner 5 (5% of network hashrate) found 2582 blocks i.e. 4.97423% of blocks. Stale rate: 1.81222%.
  - Miner 6 (3% of network hashrate) found 1548 blocks i.e. 2.98257% of blocks. Stale rate: 1.87599%.
  - Miner 7 (1% of network hashrate) found 515 blocks i.e. 0.993439% of blocks. Stale rate: 1.93876%.
  - Miner 8 (1% of network hashrate) found 515 blocks i.e. 0.993658% of blocks. Stale rate: 1.94125%.
```

## Large minority miner following the selfish mining strategy

Observe how one miner following the selfish mining strategy significantly increases the number of
stale blocks. Past a certain threshold of controlled network hashrate the selfish miner inflicts
more stale blocks on its competition than on itself, leading to an inflated "effective" hashrate.

In this example we run the simulation with a similar setup as in the previous section, except we
bump the larger miner's hashrate from 30% to 40% and make them adopt the "selfish mining" strategy.
It leads to this miner finding about 48.4% of blocks, increasing its share of blocks by ~21% at the
expense of all other miners.

This strategy only becomes profitable after a difficulty adjustment, but it's important to highlight
the attacking miner **incurs no additional energy expenditure**. In our example, for profit margins
of 2% before the attack, the **attacker increases its profit 11-fold**. Conversely, miners hit by a
dramatic reduction in the share of blocks found conserve the same energy expenditure as before the
attack, and would likely go out of business.

```
Running 32768 simulations in parallel using 32 threads.
100% progress..
After running 32768 simulations for 365d each, on average:
  - Miner 0 (40% of network hashrate) found 17927 blocks i.e. 48.4012% of blocks. Stale rate: 14.7742%. ('selfish mining' strategy)
  - Miner 1 (20% of network hashrate) found 6372 blocks i.e. 17.2035% of blocks. Stale rate: 39.4123%.
  - Miner 2 (10% of network hashrate) found 3185 blocks i.e. 8.59944% of blocks. Stale rate: 39.4438%.
  - Miner 3 (10% of network hashrate) found 3185 blocks i.e. 8.59873% of blocks. Stale rate: 39.4406%.
  - Miner 4 (10% of network hashrate) found 3185 blocks i.e. 8.5996% of blocks. Stale rate: 39.4398%.
  - Miner 5 (5% of network hashrate) found 1592 blocks i.e. 4.29905% of blocks. Stale rate: 39.4545%.
  - Miner 6 (3% of network hashrate) found 955 blocks i.e. 2.57852% of blocks. Stale rate: 39.4647%.
  - Miner 7 (1% of network hashrate) found 318 blocks i.e. 0.859907% of blocks. Stale rate: 39.4444%.
  - Miner 8 (1% of network hashrate) found 318 blocks i.e. 0.860109% of blocks. Stale rate: 39.4413%.
```

# Unit tests

Some very basic unit tests are present in [`test.cpp`](test.cpp). This file requires C++23 and libc++ to compile at the moment.
