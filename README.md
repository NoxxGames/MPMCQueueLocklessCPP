# C++ Lockless MP/MC Queue
A simple Multi-Producer, Multi-Consumer Queue Type. Written in Cpp using
just atomics with memory barriers to keep everything ordered appropriately.

Inspiration is taken from the LMAX Disruptor. If you're unfamiliar with lockless
programming and/or the usage of atomics with memory barriers, then the Disruptor
is a great place to start learning.

## Usage
First create a new queue as follows:
```c++
#include "LocklessMPMCQueue.h"

// Create a new queue of type int, with a desired size of 1500
// size is automatically rounded up to the nearest power of two.
// so the queue size will be 2048 in this case.
lockless_mpmc_queue<int, 1500> my_queue;
```
Then to add a new element:
```c++
my_queue.push(5);
```
To claim an element:
```c++
int my_integer = 0;
my_queue.pop(MyInteger);
```
