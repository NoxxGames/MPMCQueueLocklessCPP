# C++ MPMCQueue
A simple Multi-Producer, Multi-Consumer Queue Type. Written in Cpp.

Repo is new as of 02-December 2021, will have documentation coming when I get the time.

## Usage
First create a new queue as follows:
```c++
TMPMCQueue<int> MyQueue;
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
The below code shows a very simple example with just one producer and one consumer.
Adjust the TIMES_TO_CYCLE macro to make the benchmark run for longer. 
```c++
#include "MPMCQueue.h"

#include <thread>

/** Define how many times to Produce/Consume an element. */
#define TIMES_TO_CYCLE 1000

std::atomic<bool> ProducerFinished = {false};
std::atomic<bool> ConsumerFinished = {false};

int main()
{
    TMPMCQueue<int> MyQueue;

    /** Producer Thread */
    std::thread([&]()
    {
        const int NumberToAddLoads = 100;
        for(int i = 0; i < TIMES_TO_CYCLE; i++)
        {
            MyQueue.Enqueue(NumberToAddLoads);
        }

        ProducerFinished.store(true);
    }).detach();

    /** Consumer Thread */
    std::thread([&]()
    {
        int32 NumberToHoldLoads = 0;
        for(int i = 0; i < TIMES_TO_CYCLE; i++)
        {
            MyQueue.Dequeue(NumberToHoldLoads);
        }
        ConsumerFinished.store(true);
    }).detach();

    /** Spin wait until the two threads are complete */
    while(!ProducerFinished.load(std::memory_order_relaxed) ||
        !ConsumerFinished.load(std::memory_order_relaxed))
    { }
    
    system("pause");
    return 0;
}
```
