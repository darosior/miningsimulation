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
RNG than the default one from the STL.

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
Bitcoin network in 2025. In the first run, block propagation is set to 10 seconds. In the second run
it is set to 100 milliseconds.

Block propagation set to 10s:
```
Running 32768 simulations in parallel using 16 threads.
100% progress..
After running 32768 simulations for 365d each, on average:
  - Miner 0 (30% of network hashrate) found 15621 blocks i.e. 30.0901% of blocks. Stale rate: 1.0092%.
  - Miner 1 (29% of network hashrate) found 15096 blocks i.e. 29.0794% of blocks. Stale rate: 1.04315%.
  - Miner 2 (12% of network hashrate) found 6210 blocks i.e. 11.9624% of blocks. Stale rate: 1.62079%.
  - Miner 3 (11% of network hashrate) found 5690 blocks i.e. 10.962% of blocks. Stale rate: 1.65404%.
  - Miner 4 (8% of network hashrate) found 4134 blocks i.e. 7.96423% of blocks. Stale rate: 1.75598%.
  - Miner 5 (5% of network hashrate) found 2582 blocks i.e. 4.97405% of blocks. Stale rate: 1.85974%.
  - Miner 6 (3% of network hashrate) found 1547 blocks i.e. 2.9815% of blocks. Stale rate: 1.92927%.
  - Miner 7 (1% of network hashrate) found 515 blocks i.e. 0.993098% of blocks. Stale rate: 1.99286%.
  - Miner 8 (1% of network hashrate) found 515 blocks i.e. 0.993211% of blocks. Stale rate: 1.99886%.
```

Block propagation set to 100ms:
```
Running 32768 simulations in parallel using 16 threads.
100% progress..
After running 32768 simulations for 365d each, on average:
  - Miner 0 (30% of network hashrate) found 15776 blocks i.e. 30.0008% of blocks. Stale rate: 0.0101929%.
  - Miner 1 (29% of network hashrate) found 15251 blocks i.e. 29.0011% of blocks. Stale rate: 0.0105712%.
  - Miner 2 (12% of network hashrate) found 6310 blocks i.e. 11.9999% of blocks. Stale rate: 0.0162978%.
  - Miner 3 (11% of network hashrate) found 5784 blocks i.e. 11.0001% of blocks. Stale rate: 0.0168355%.
  - Miner 4 (8% of network hashrate) found 4207 blocks i.e. 8.00004% of blocks. Stale rate: 0.0176048%.
  - Miner 5 (5% of network hashrate) found 2629 blocks i.e. 4.99955% of blocks. Stale rate: 0.0190155%.
  - Miner 6 (3% of network hashrate) found 1577 blocks i.e. 2.99928% of blocks. Stale rate: 0.0193449%.
  - Miner 7 (1% of network hashrate) found 525 blocks i.e. 0.999467% of blocks. Stale rate: 0.0196773%.
  - Miner 8 (1% of network hashrate) found 525 blocks i.e. 0.99974% of blocks. Stale rate: 0.0204597%.
```

## Large minority miner following the selfish mining strategy

Observe how one miner following the selfish mining strategy significantly increases the number of
stale blocks. Past a certain threshold of controlled network hashrate the selfish miner inflicts
more stale blocks on its competition than on itself, leading to an inflated "effective" hashrate.

In this example we run the simulation with a similar setup as in the previous section, except we
bump the larger miner's hashrate from 30% to 40% and make them adopt the "selfish mining" strategy.
It leads to this miner finding about 46.5% of blocks, increasing its revenues by ~16% at the expense
of all other miners.

```
Running 32768 simulations in parallel using 16 threads.
100% progress..
After running 32768 simulations for 365d each, on average:
  - Miner 0 (40% of network hashrate) found 16502 blocks i.e. 46.6844% of blocks. Stale rate: 27.4658%. ('selfish mining' strategy)
  - Miner 1 (19% of network hashrate) found 5970 blocks i.e. 16.8889% of blocks. Stale rate: 67.4269%.
  - Miner 2 (12% of network hashrate) found 3769 blocks i.e. 10.6612% of blocks. Stale rate: 67.498%.
  - Miner 3 (11% of network hashrate) found 3455 blocks i.e. 9.77431% of blocks. Stale rate: 67.4999%.
  - Miner 4 (8% of network hashrate) found 2512 blocks i.e. 7.10735% of blocks. Stale rate: 67.5386%.
  - Miner 5 (5% of network hashrate) found 1570 blocks i.e. 4.44231% of blocks. Stale rate: 67.5667%.
  - Miner 6 (3% of network hashrate) found 942 blocks i.e. 2.66464% of blocks. Stale rate: 67.6207%.
  - Miner 7 (1% of network hashrate) found 314 blocks i.e. 0.888353% of blocks. Stale rate: 67.7416%.
  - Miner 8 (1% of network hashrate) found 314 blocks i.e. 0.888536% of blocks. Stale rate: 67.7529%.
```

# Unit tests

Some very basic unit tests are present in [`test.cpp`](test.cpp). This file requires C++23 and libc++ to compile at the moment.
