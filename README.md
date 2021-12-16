# C++ Lockless MP/MC Queue
A simple Multi-Producer, Multi-Consumer Queue Type. Written in Cpp using
just atomics with memory barriers to keep everything ordered appropriately.

Inspiration is taken from the LMAX Disruptor. If you're unfamiliar with lockless
programming and/or the usage of atomics with memory barriers, then the Disruptor
is a great place to start learning.

## How it Works

#### Bounded Ring Buffer
The queue uses a bounded ring buffer to store the data. In order to avoid the use
of the modulo operator for calculating the index, the queue must have a size which is equal to a power of two.
This allows us to use a bitmask instead, which is more efficient.
#### Two Cursors
A "Producer" curosr, and a "Consumer" cursor are used to indicate to the next incoming
producer/consumer which index on the ring buffer they will be accessing. When a producer/consumer is attempting access,
they first increment their respective cursor, then access the buffer. By doing the incrementation first then they will be effectively
claiming that index, ensuring that only they are accessing that particular index on the buffer. This way each index in the buffer is guarded against multiple access attempts at the same time.
Since multiple accessors may try  to increment the cursor at the same time, they first
enter a busy spin and via a CAS operation they verify that they were
successful in incrementing the cursor, and not someone else.
#### TSequentialContainer Type
The queue relies on a Sequential Container which uses memory barriers to synchronize
access to an atomic variable of template type "T".
Both of the cursors that the buffer uses are of type FSequentialInteger, which derives from the 
TSequentialContainer class. This is the type that all access to the ring buffer is protected through.


## Usage
First create a new queue as follows:
```c++
#include "MPMCQueue.h"

TMPMCQueue<int> MyQueue;
// Or with a manually specified size (default is 1024),
// size is automatically rounded up to the nearest power of two!!!
TMPMCQueue<int, 2048> MyQueue;
```
Then to add a new element:
```c++
MyQueue.Enqueue(5);
```
To claim an element:
```c++
int MyInteger = 0;
MyQueue.Dequeue(MyInteger);
```
The claimed result will be stored in "MyInteger", in this case.

## Example Benchmark code
The below code shows a very simple example with two producers and consumers.
Adjust the TIMES_TO_CYCLE macro to make the benchmark run for longer. 
```c++
#include "MPMCQueue.h"

#include <thread>

/** Define how many times to Produce/Consume an element. */
#define TIMES_TO_CYCLE 100000000

std::atomic<bool> ProducerFinished1 = {false};
std::atomic<bool> ConsumerFinished1 = {false};

std::atomic<bool> ProducerFinished2 = {false};
std::atomic<bool> ConsumerFinished2 = {false};

int main()
{
    TMPMCQueue<int, 4194304> MyQueue;

    /** Producer 1 Thread */
    std::thread([&]()
    {
        const int NumberToAddLoads = 100;
        for(int i = 0; i < TIMES_TO_CYCLE; i++)
        {
            MyQueue.Enqueue(NumberToAddLoads);
        }

        ProducerFinished1.store(true);
    }).detach();

    /** Producer 2 Thread */
    std::thread([&]()
    {
        const int NumberToAddLoads = 100;
        for(int i = 0; i < TIMES_TO_CYCLE; i++)
        {
            MyQueue.Enqueue(NumberToAddLoads);
        }

        ProducerFinished2.store(true);
    }).detach();

    /** Consumer 1 Thread */
    std::thread([&]()
    {
        int32 NumberToHoldLoads = 0;
        for(int i = 0; i < TIMES_TO_CYCLE; i++)
        {
            MyQueue.Dequeue(NumberToHoldLoads);
        }
        ConsumerFinished1.store(true);
    }).detach();

    /** Consumer 2 Thread */
    std::thread([&]()
    {
        int32 NumberToHoldLoads = 0;
        for(int i = 0; i < TIMES_TO_CYCLE; i++)
        {
            MyQueue.Dequeue(NumberToHoldLoads);
        }
        ConsumerFinished2.store(true);
    }).detach();

    /** Spin yield until the four threads are complete */
    while(!ProducerFinished1.load(std::memory_order_relaxed) ||
        !ConsumerFinished1.load(std::memory_order_relaxed) ||
        !ProducerFinished2.load(std::memory_order_relaxed) ||
        !ConsumerFinished2.load(std::memory_order_relaxed))
    {
        std::this_thread::yield();
    }
    
    system("pause");
    return 0;
}
```

## TODO

Presently there is no good solution to the ABA problem in the queue. Mainly because I haven't had time yet, but
my current line of thinking is to do something similar to the LMAX Disruptor. Where the amount of producers, and
the amount of consumers is tracked. This allows for new producers & consumers to coordinate with each other
as they obtain and release access to the two cursors, making sure they don't step over each other as per the ABA problem.
