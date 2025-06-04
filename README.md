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

## Impact of block propagation on centralization pressure

Observe how stale blocks inflate the effective hashrate of large miners at the expense of smaller
ones, and how reducing block propagation time helps restore a level playing field.

In this example, the simulation is ran twice for a year with 5 miners (1 with 10% of the hashrate, 3
with 20% and 1 with 30%). In the first run, block propagation is set to 10 seconds. In the second
run it is set to 100 milliseconds.

Block propagation set to 10s:
```
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

Block propagation set to 100ms:
```
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

## Large minority miner following the selfish mining strategy

Observe how one miner following the selfish mining strategy significantly increases the number of
stale blocks. Past a certain threshold of controlled network hashrate the selfish miner inflicts
more stale blocks on its competition than on itself, leading to an inflated "effective" hashrate.

In this example we run the simulation for a year with 5 miners. One with 10% of the network
hashrate, two with 15%, one with 20% and the last one with 40%. The miner with 40% of the network
hashrate is set to follow a selfish mining strategy, leading it to find about 46.5% of blocks. This
is a ~16% increase in revenue for this miner, at the expense of smaller miner.

```
After 31449600s (364d) and 35132 blocks found:
  - Miner 0 (10% of network hashrate) found 3164 blocks i.e. 9.00603% of blocks. Stale rate: 67.2882%.
  - Miner 1 (15% of network hashrate) found 4606 blocks i.e. 13.1106% of blocks. Stale rate: 66.9996%.
  - Miner 2 (15% of network hashrate) found 4785 blocks i.e. 13.6201% of blocks. Stale rate: 66.813%.
  - Miner 3 (20% of network hashrate) found 6172 blocks i.e. 17.568% of blocks. Stale rate: 68.1627%.
  - Miner 4 (40% of network hashrate) found 16407 blocks i.e. 46.701% of blocks. Stale rate: 28.0063%. ('selfish mining' strategy)


After 31536000s (365d) and 35224 blocks found:
  - Miner 0 (10% of network hashrate) found 3167 blocks i.e. 8.99103% of blocks. Stale rate: 67.3824%.
  - Miner 1 (15% of network hashrate) found 4614 blocks i.e. 13.099% of blocks. Stale rate: 67.1651%.
  - Miner 2 (15% of network hashrate) found 4791 blocks i.e. 13.6015% of blocks. Stale rate: 67.0841%.
  - Miner 3 (20% of network hashrate) found 6183 blocks i.e. 17.5534% of blocks. Stale rate: 68.3972%.
  - Miner 4 (40% of network hashrate) found 16473 blocks i.e. 46.7664% of blocks. Stale rate: 27.9427%. ('selfish mining' strategy)
```
