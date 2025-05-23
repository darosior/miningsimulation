# Block propagation simulator

A simple block propagation simulation. Create a network with a given number of miners, each with its
own proportion of the network hashrate and block propagation time. The block propagation time is a
simplification: before the threshold no other miner has seen a block, after it all miners have seen
it. The simulation proceeds by advancing one millisecond at a time from the start, responding to
events such as new block found, block previously found propagated to the rest of the network, etc.
The time between blocks is drawn from an exponential distribution with mean 600 seconds. Which miner
found the last block is drawn randomly based on each miner's proportion of the network hashrate.

One goal of this simulation is to be simple yet close to reality. The simplified block propagation
modeling is an example of that. I do intend to extend it in the future by implementing a simple
selfish mining strategy (only in the worst case i.e. $\gamma = 0$ according to the 2013 paper).

Thanks to Clara Shikhelman for suggesting a "more obviously correct" way of drawing block interval
and attributions. Thanks to Pieter Wuille for suggesting to use and providing the code for a faster
RNG than the default one from the STL.

# Running the simulation

You will need a C++ compiler compatible with C++20 (any remotely modern C++ compiler will do).
That's it. For instance with clang 19:
```
clang++-19 -O3 -std=c++20 main.cpp -o simulation
```

Running the program will run the simulation forever:
```
./simulation
```
# Example results

As an example consider these two results of running the simulation for a year with 5 miners, 1 with
10% of the hashrate, 3 with 20% and 1 with 30%.

Block propagation set to 10s (for all miners):
```
After 31363200s (363d):
  - Miner 0 (10% of network hashrate) found 9.45779% of blocks. Stale rate: 2.03874%.
  - Miner 1 (20% of network hashrate) found 20.1072% of blocks. Stale rate: 1.07403%.
  - Miner 2 (20% of network hashrate) found 20.246% of blocks. Stale rate: 1.33333%.
  - Miner 3 (20% of network hashrate) found 20.1554% of blocks. Stale rate: 1.2628%.
  - Miner 4 (30% of network hashrate) found 30.0336% of blocks. Stale rate: 1.0529%.


After 31449600s (364d):
  - Miner 0 (10% of network hashrate) found 9.45657% of blocks. Stale rate: 2.05452%.
  - Miner 1 (20% of network hashrate) found 20.1154% of blocks. Stale rate: 1.07105%.
  - Miner 2 (20% of network hashrate) found 20.2405% of blocks. Stale rate: 1.34005%.
  - Miner 3 (20% of network hashrate) found 20.15% of blocks. Stale rate: 1.26014%.
  - Miner 4 (30% of network hashrate) found 30.0375% of blocks. Stale rate: 1.05027%.


After 31536000s (365d):
  - Miner 0 (10% of network hashrate) found 9.45217% of blocks. Stale rate: 2.07065%.
  - Miner 1 (20% of network hashrate) found 20.1055% of blocks. Stale rate: 1.06891%.
  - Miner 2 (20% of network hashrate) found 20.2418% of blocks. Stale rate: 1.33662%.
  - Miner 3 (20% of network hashrate) found 20.1727% of blocks. Stale rate: 1.25559%.
  - Miner 4 (30% of network hashrate) found 30.0297% of blocks. Stale rate: 1.05431%.
```

Block propagation set to 100ms (for all miners):
```
After 31363200s (363d):
  - Miner 0 (10% of network hashrate) found 10.0276% of blocks. Stale rate: 0%.
  - Miner 1 (20% of network hashrate) found 19.9919% of blocks. Stale rate: 0%.
  - Miner 2 (20% of network hashrate) found 20.0897% of blocks. Stale rate: 0%.
  - Miner 3 (20% of network hashrate) found 20.0226% of blocks. Stale rate: 0.00957579%.
  - Miner 4 (30% of network hashrate) found 29.8681% of blocks. Stale rate: 0.0128386%.


After 31449600s (364d):
  - Miner 0 (10% of network hashrate) found 10.031% of blocks. Stale rate: 0%.
  - Miner 1 (20% of network hashrate) found 19.9874% of blocks. Stale rate: 0%.
  - Miner 2 (20% of network hashrate) found 20.0868% of blocks. Stale rate: 0%.
  - Miner 3 (20% of network hashrate) found 20.018% of blocks. Stale rate: 0.00955201%.
  - Miner 4 (30% of network hashrate) found 29.8769% of blocks. Stale rate: 0.0128%.


After 31536000s (365d):
  - Miner 0 (10% of network hashrate) found 10.0353% of blocks. Stale rate: 0%.
  - Miner 1 (20% of network hashrate) found 19.9828% of blocks. Stale rate: 0%.
  - Miner 2 (20% of network hashrate) found 20.0858% of blocks. Stale rate: 0%.
  - Miner 3 (20% of network hashrate) found 20.0267% of blocks. Stale rate: 0.0095229%.
  - Miner 4 (30% of network hashrate) found 29.8694% of blocks. Stale rate: 0.0127698%.
```
