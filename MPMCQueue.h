// SPDX-License-Identifier: GPL-2.0-or-later
/** Lockless Multi-Producer Multi-Consumer Queue Type.
 * Author: Primrose Taylor
 */

#ifndef MPMCQUEUE_H
#define MPMCQUEUE_H

#include "stdio.h"
#include "stdlib.h"

#include <atomic>

#define PLATFORM_CACHE_LINE_SIZE 64
#define MPMC_PADDING(_VAR_NAME_) alignas(PLATFORM_CACHE_LINE_SIZE) uint8_t _VAR_NAME_[PLATFORM_CACHE_LINE_SIZE] = { }

/**
 * A container which can ensure that access to it's data will be sequentially consistent across all accessing threads,
 * but allows for getting the data via a custom memory order.
 */
template <typename T>
class TSequentialContainer
{
public:
    TSequentialContainer()
    {
        static_assert(
            std::is_copy_constructible_v<T>	    ||
            std::is_copy_assignable_v<T>		||
            std::is_move_assignable_v<T>		||
            std::is_move_constructible_v<T>,
            "Can't use non-copyable, non-assignable, non-movable, or non-constructible type!"
        );
    }

    explicit TSequentialContainer(const T& InitialValue)
    {
        TSequentialContainer();
        Data.store(InitialValue, std::memory_order_seq_cst);
    }
    
    /**
     * Get the data, using an acquire fence to ensure that any prior write is visible to this load.
     */
    T Get() const
    {
        const T OutCopy = Data.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return OutCopy;
    }

    /**
     * Load the data with relaxed semantics. NOTE: NOT THREAD SAFE!
     */
    T GetRelaxed() const
    {
        return Data.load(std::memory_order_relaxed);
    }
    
    T GetCustom(const std::memory_order MemoryOrder) const
    {
        return Data.load(MemoryOrder);
    }
    
    /**
     * Set the data, first performing a release fence.
     * The release fence will ensure that any subsequent read will see this write.
     */
    void Set(const T& NewData) 
    {
        std::atomic_thread_fence(std::memory_order_release);
        Data.store(NewData, std::memory_order_relaxed);
    }
    
    /**
     * Set the data by first performing a release fence, then storing the data,
     * then performing a full fence.
     */
    void SetFullFence(const T& NewData)
    {
        std::atomic_thread_fence(std::memory_order_release);
        Data.store(NewData, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    
    void SetCustom(const T& NewData, const std::memory_order MemoryOrder)
    {
        Data.store(NewData, MemoryOrder);
    }
    
    /**
     * Perform a CAS operation on the stored data.
     * Uses release semantics if works.
     * Uses relaxed semantics if failed.
     */
    bool CompareAndSet(T& Expected, const T& NewValue)
    {
        return Data.compare_exchange_weak(Expected, NewValue,
            std::memory_order_release, std::memory_order_relaxed);
    }
    
protected:
    MPMC_PADDING(Pad1);
    /**
     * An atomic variable which holds the data.
     */
    std::atomic<T> Data;
    MPMC_PADDING(Pad2);

private:
    TSequentialContainer(const TSequentialContainer&) = delete;
    TSequentialContainer& operator=(const TSequentialContainer&) = delete;
};

/**
 * A simple child class of the @link TSequentialContainer which uses an int64 instead of a template.
 * Providing some extra functions specific to modifying an integer.
 */
class FSequentialInteger : public TSequentialContainer<int_fast64_t>
{
public:
    FSequentialInteger(const int_fast64_t InitialValue = 0)
        : TSequentialContainer()
    {
        SetFullFence(InitialValue);
    }
    
    /**
     * Uses a fetch_add with Acquire/Release semantics to increment the integer.
     *
     * @return Returns the original value of the integer.
     */
    int_fast64_t AddAndGetOldValue(const int_fast64_t Value)
    {
        return Data.fetch_add(Value, std::memory_order_acq_rel);
    }
    
    /**
     * @link AddAndGetOldValue()
     */
    int_fast64_t AddAndGetNewValue(const int_fast64_t Value)
    {
        return AddAndGetOldValue(Value) + Value;
    }

    /**
     * @link AddAndGetNewValue()
     * @link AddAndGetOldValue()
     */
    int_fast64_t IncrementAndGetOldValue()
    {
        return AddAndGetOldValue(1);
    }

    /**
     * @link IncrementAndGetOldValue()
     */
    void Increment()
    {
        IncrementAndGetOldValue();
    }

    void IncrementRelaxed()
    {
        Data.fetch_add(1, std::memory_order_relaxed);
    }

    void operator=(const int_fast64_t NewValue)
    {
        SetFullFence(NewValue);
    }
};

/**
 * Enum used to represent each status output from the Enqueue/Dequeue functions inside @link TMPMCQueue
 */
enum class EMPMCQueueErrorStatus : uint_fast8_t
{
    TRANSACTION_SUCCESS,
    BUFFER_FULL,
    BUFFER_EMPTY,
    BUFFER_NOT_INITIALIZED,
    COPY_FAILED,
    COPY_SUCCESS,
    BUFFER_COPY_FAILED,
    BUFFER_COPY_SUCCESS
};

/**
 * A Lockless Multi-Producer, Multi-Consumer Queue that uses
 * a bounded ring buffer to store the data. All access to the ring buffer
 * is guarded by the use of two cursors, which use memory barriers and
 * a fetch_add to synchronize access to the ring buffer.
 *
 * @link TSequentialContainer A sequential container of type T, which uses memory barriers to sync access to it's data.
 * @link FSequentialInteger A sequential integer container, which uses memory barriers to sync access to it's data.
 * @link EMPMCQueueErrorStatus Enum used to represent each status output from the Enqueue/Dequeue functions.
 *
 * @template T The type to use for the queue.
 * @template TQueueSize The size you want the queue to be. This will be rounded UP to the nearest power of two.
 *
 * @biref A Lockless Multi-Producer, Multi-Consumer Queue.
 */
template <typename T, uint_fast64_t TQueueSize>
class TMPMCQueue final
{
private:
    using FElementType = T;
    using FCursor = FSequentialInteger;

public:
    TMPMCQueue()
    {
        if(TQueueSize == 0 || TQueueSize == UINT64_MAX)
        {
            return;
        }

        /**
         * Ceil the queue size to the nearest power of 2
         * @cite https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
         */
        uint_fast64_t NearestPower = TQueueSize;
        {
            NearestPower--;
            NearestPower |= NearestPower >> 1; // 2 bit
            NearestPower |= NearestPower >> 2; // 4 bit
            NearestPower |= NearestPower >> 4; // 8 bit
            NearestPower |= NearestPower >> 8; // 16 bit
            NearestPower |= NearestPower >> 16; // 32 bit
            NearestPower |= NearestPower >> 32; // 64 bit
            NearestPower++;
        }

        IndexMask.store(NearestPower - 1); // Set the IndexMask to be one less than the NearestPower

        /** Allocate the ring buffer. */
        RingBuffer = (FBufferNode*)calloc(NearestPower, sizeof(FBufferNode));
        for(uint_fast64_t i = 0; i < NearestPower; ++i)
        {
            RingBuffer[i].Data = (FElementType*)malloc(sizeof(FElementType));
        }
        
        ConsumerCursor.SetFullFence(0);
        ProducerCursor.SetFullFence(0);
    }

    ~TMPMCQueue()
    {
        if(RingBuffer == nullptr)
            return;
        
        free(RingBuffer);
        RingBuffer = nullptr;
    }

    /**
     * Add a new element to the queue.
     *
     * @link Dequeue()
     * @link FSequentialInteger::Get()
     * @link TSequentialContainer::IncrementAndGetOldValue()
     * @param NewElement The new element to add to the queue.
     *
     * @return An error status, used to check if the add worked.
     */
    EMPMCQueueErrorStatus Enqueue(const FElementType& NewElement)
    {
        /** Get the Consumer & Producer cursor values, using an acquire fence */
        const int_fast64_t CurrentConsumerCursor = ConsumerCursor.Get();
        const int_fast64_t CurrentProducerCursor = ProducerCursor.Get();
        
        /** Return false if the buffer is full */
        if((CurrentProducerCursor + 1) == CurrentConsumerCursor)
        {
            return EMPMCQueueErrorStatus::BUFFER_FULL;
        }
        
        const int_fast64_t ClaimedIndex = ProducerCursor.IncrementAndGetOldValue(); // fetch_add
        const int_fast64_t ClaimedIndexMask = ClaimedIndex & IndexMask.load(std::memory_order_relaxed);
        
        /** Update the index on the ring buffer with the new element */
        *RingBuffer[ClaimedIndexMask].Data = NewElement;
        
        return EMPMCQueueErrorStatus::TRANSACTION_SUCCESS;
    }

    EMPMCQueueErrorStatus EnqueueCAS(const FElementType& NewElement)
    {
        /** Get the Consumer & Producer cursor values, using an acquire fence */
        const int_fast64_t CurrentConsumerCursor = ConsumerCursor.Get();
        const int_fast64_t CurrentProducerCursor = ProducerCursor.Get();
        
        /** Return false if the buffer is full */
        if((CurrentProducerCursor + 1) == CurrentConsumerCursor)
        {
            return EMPMCQueueErrorStatus::BUFFER_FULL;
        }
        
        int_fast64_t ClaimedIndex = CurrentProducerCursor;
        
        while(!ProducerCursor.CompareAndSet(ClaimedIndex, ClaimedIndex + 1))
        {
            ClaimedIndex = ProducerCursor.Get();
            _mm_pause();
        }
        
        const int_fast64_t ThisIndexMask = ClaimedIndex & IndexMask;
        
        /** Update the index on the ring buffer with the new element */
        *RingBuffer[ThisIndexMask].Data = NewElement;
        
        return EMPMCQueueErrorStatus::TRANSACTION_SUCCESS;
    }
    
    /**
     * Claim an element from the queue.
     *
     * @link Enqueue()
     * @link FSequentialInteger::Get()
     * @link TSequentialContainer::IncrementAndGetOldValue()
     * @param Output A reference to the variable to store the output in.
     *
     * @link EMPMCQueueErrorStatus
     * @return An error status, used to check if the add worked.
     */
    EMPMCQueueErrorStatus Dequeue(FElementType& Output)
    {
        /** Get the Consumer & Producer cursor values, using an acquire fence */
        const int_fast64_t CurrentConsumerCursor = ConsumerCursor.Get();
        const int_fast64_t CurrentProducerCursor = ProducerCursor.Get();

        // Check if the buffer is empty
        if(CurrentConsumerCursor == CurrentProducerCursor)
        {
            return EMPMCQueueErrorStatus::BUFFER_EMPTY;
        }

        /** Perform a fetch_add with acquire_release semantics */
        const int_fast64_t ClaimedIndex = ConsumerCursor.IncrementAndGetOldValue();
        /** Calculate the index, avoiding the use of modulo */
        const int_fast64_t ClaimedIndexMask = ClaimedIndex & IndexMask.load(std::memory_order_relaxed);
        
        /** Store the claimed element from the ring buffer in the Output var */
        Output = *RingBuffer[ClaimedIndexMask].Data;
        
        return EMPMCQueueErrorStatus::TRANSACTION_SUCCESS;
    }

    EMPMCQueueErrorStatus DequeueCAS(FElementType& Output)
    {
        const int_fast64_t CurrentConsumerCursor = ConsumerCursor.Get();
        const int_fast64_t CurrentProducerCursor = ProducerCursor.Get();
        
        if(CurrentConsumerCursor == CurrentProducerCursor)
        {
            return EMPMCQueueErrorStatus::BUFFER_EMPTY;
        }
        
        int_fast64_t ClaimedIndex = CurrentConsumerCursor;

        while(!ConsumerCursor.CompareAndSet(ClaimedIndex, ClaimedIndex + 1))
        {
            ClaimedIndex = ConsumerCursor.Get();
            _mm_pause();
        }
        
        const int_fast64_t ThisIndexMask = ClaimedIndex & IndexMask.load(std::memory_order_relaxed);
        
        /** Update the index on the ring buffer with the new element */
        Output = *RingBuffer[ThisIndexMask].Data;
        
        return EMPMCQueueErrorStatus::TRANSACTION_SUCCESS;
    }

private:
    struct FBufferNode
    {
        FBufferNode() noexcept
            : Data(nullptr)
        {
        }
        
        MPMC_PADDING(Pad1);
        FElementType* Data;
        MPMC_PADDING(Pad2);
    };
    
private:
    MPMC_PADDING(Pad1);
    /** Stores a value that MUST be one less than a power of two e.g 1023.
    * Used to calculate an index for access to the @link RingBuffer.
    */
    std::atomic<uint_fast64_t>                  IndexMask; 
    MPMC_PADDING(Pad2);
    /**
     * This is the pointer to the ring buffer which holds the queue's data.
     * This is allocated in the default constructor using calloc.
     */
    FBufferNode*                                RingBuffer;
    MPMC_PADDING(Pad3);
    /**
     * The cursor that holds the next available index on the ring buffer for Consumers.
     */
    FCursor                                     ConsumerCursor;
    MPMC_PADDING(Pad4);
    /**
     * The cursor that holds the next available index on the ring buffer for Producers.
     */
    FCursor                                     ProducerCursor;
    MPMC_PADDING(PadThai);

private:
    TMPMCQueue(const TMPMCQueue&) = delete;
    TMPMCQueue& operator=(const TMPMCQueue&) = delete;
};

#endif // MPMCQUEUE_H
