// SPDX-License-Identifier: GPL-2.0-or-later
/** Lockless Multi-Producer Multi-Consumer Queue Type.
 * Author: Primrose Taylor
 */

#ifndef MPMCQUEUE_H
#define MPMCQUEUE_H

#include <atomic>

typedef signed char        int8;
typedef short              int16;
typedef int                int32;
typedef long long          int64;
typedef unsigned char      uint8;
typedef unsigned short     uint16;
typedef unsigned int       uint32;
typedef unsigned long long uint64;

#define PLATFORM_CACHE_LINE_SIZE 64
#define MPMC_ALIGNMENT alignas(PLATFORM_CACHE_LINE_SIZE)
#define MPMC_PADDING MPMC_ALIGNMENT uint8

#define SPIN_WAIT_COUNT 1000

#if defined(_WIN32) || defined(__CYGWIN__)
#define FORCEINLINE __forceinline
#else
#define FORCEINLINE inline
#endif

/**
 * Static utility library for working with memory.
 */
class UMemoryStatics
{
public:
    /** Contiguous memory allocation. All elements initialised to zero. */
    template<typename T>
    static FORCEINLINE T* Calloc(const uint64 AllocationSize)
    {
        return (T*)calloc(AllocationSize, sizeof(T));
    }
};

/**
 * utility template for a class that should not be copyable.
 * Derive from this class to make your class non-copyable
 */
class FNoncopyable
{
    /** @cite Taken from the Unreal Engine 4 Source code.
     *  @ref <FileName> UnrealTemplate.h
     */
    
protected:
    // ensure the class cannot be constructed directly
    FNoncopyable() {}
    // the class should not be used in a polymorphic manner
    ~FNoncopyable() {}
private:
    FNoncopyable(const FNoncopyable&);
    FNoncopyable& operator=(const FNoncopyable&);
};

#define SEQUENCE_ERROR_VALUE -2

/**
 * A container which can ensure that access to it's data will be sequentially consistent across all accessing threads.
 */
template <typename T>
class MPMC_ALIGNMENT TSequentialContainer : public FNoncopyable
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
    FORCEINLINE T Get() const
    {
        const T OutCopy = Data.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return OutCopy;
    }

    /**
     * Load the data with relaxed semantics. NOTE: NOT THREAD SAFE!
     */
    FORCEINLINE T GetRelaxed() const
    {
        return Data.load(std::memory_order_relaxed);
    }
    
    /**
     * Set the data, first performing a release fence.
     * The release fence will ensure that any subsequent read will see this write.
     */
    FORCEINLINE void Set(const T& NewData)
    {
        std::atomic_thread_fence(std::memory_order_release);
        Data.store(NewData, std::memory_order_relaxed);
    }
    
    /**
     * Set the data by first performing a release fence, then storing the data,
     * then performing a full fence.
     */
    FORCEINLINE void SetFullFence(const T& NewData)
    {
        std::atomic_thread_fence(std::memory_order_release);
        Data.store(NewData, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    
    /**
     * Perform a CAS operation on the stored data.
     * Uses release semantics if works.
     * Uses relaxed semantics if failed.
     */
    FORCEINLINE bool CompareAndSet(T& Expected, const T& NewValue)
    {
        return Data.compare_exchange_weak(Expected, NewValue,
            std::memory_order_release, std::memory_order_relaxed);
    }
    
protected:
    MPMC_PADDING PadToAvoidContention0[PLATFORM_CACHE_LINE_SIZE] = { };
    /**
     * An atomic variable which holds the data.
     */
    MPMC_ALIGNMENT std::atomic<T> Data;
    MPMC_PADDING PadToAvoidContention1[PLATFORM_CACHE_LINE_SIZE] = { };
};

/**
 * A simple child class of the @link TSequentialContainer which uses an int64 instead of a template.
 * Providing some extra functions specific to modifying an integer.
 */
class MPMC_ALIGNMENT FSequentialInteger : public TSequentialContainer<int64>
{
public:
    FSequentialInteger(const int64 InitialValue = 0)
        : TSequentialContainer()
    {
        SetFullFence(InitialValue);
    }
    
    /**
     * Uses a fetch_add with Acquire/Release semantics to increment the integer.
     *
     * @return Returns the original value of the integer.
     */
    FORCEINLINE int64 AddAndGetOldValue(const int64 Value)
    {
        return Data.fetch_add(Value, std::memory_order_acq_rel);
    }
    
    /**
     * @link AddAndGetOldValue()
     */
    FORCEINLINE int64 AddAndGetNewValue(const int64 Value)
    {
        return AddAndGetOldValue(Value) + Value;
    }

    /**
     * @link AddAndGetNewValue()
     * @link AddAndGetOldValue()
     */
    FORCEINLINE int64 IncrementAndGetOldValue()
    {
        return AddAndGetOldValue(1);
    }
};

/**
 * Enum used to represent each status output from the Enqueue/Dequeue functions inside @link TMPMCQueue
 */
enum class EMPMCQueueErrorStatus : uint8
{
    TRANSACTION_SUCCESS,
    BUFFER_FULL,
    BUFFER_EMPTY,
    BUFFER_NOT_INITIALIZED
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
template <typename T, uint64 TQueueSize>
class TMPMCQueue final : public FNoncopyable
{
private:
    using FElementType = T;
    using FCursor = FSequentialInteger;

public:
    TMPMCQueue()
    {
        if(TQueueSize == 0 || TQueueSize > UINT64_MAX)
        {
            return;
        }

        /** @cite https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2 */
        uint64 NearestPower = 1;
        NearestPower--;
        NearestPower |= NearestPower >> 1; // 2 bit
        NearestPower |= NearestPower >> 2; // 4 bit
        NearestPower |= NearestPower >> 4; // 8 bit
        NearestPower |= NearestPower >> 8; // 16 bit
        NearestPower |= NearestPower >> 16; // 32 bit
        NearestPower |= NearestPower >> 32; // 64 bit
        NearestPower++;
        IndexMask = NearestPower - 1; // Set the IndexMask to be one less than the NearestPower

        /** Allocate the ring buffer. */
        RingBuffer = UMemoryStatics::Calloc<FElementType>(NearestPower);
        
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
     * @link CalculateIndex()
     * @param NewElement The new element to add to the queue.
     *
     * @return An error status, used to check if the add worked.
     */
    EMPMCQueueErrorStatus Enqueue(const FElementType& NewElement)
    {
        const int64 CurrentConsumerCursor = ConsumerCursor.Get();
        const int64 CurrentProducerCursor = ProducerCursor.Get();
        
        /** Return false if the buffer is full */
        if((CurrentProducerCursor + 1) == CurrentConsumerCursor)
        {
            return EMPMCQueueErrorStatus::BUFFER_FULL;
        }

        const int64 ClaimedIndex = ProducerCursor.IncrementAndGetOldValue(); // fetch_add
        
        /** Update the index on the ring buffer with the new element */
        RingBuffer[CalculateIndex(ClaimedIndex)] = NewElement;
        
        return EMPMCQueueErrorStatus::TRANSACTION_SUCCESS;
    }

    /**
     * Claim an element from the queue.
     *
     * @link Enqueue()
     * @link FSequentialInteger::Get()
     * @link TSequentialContainer::IncrementAndGetOldValue()
     * @link CalculateIndex()
     * @param Output A reference to the variable to store the output in.
     *
     * @link EMPMCQueueErrorStatus
     * @return An error status, used to check if the add worked.
     */
    EMPMCQueueErrorStatus Dequeue(FElementType& Output)
    {
        const int64 CurrentConsumerCursor = ConsumerCursor.Get();
        const int64 CurrentProducerCursor = ProducerCursor.Get();

        if(CurrentConsumerCursor == CurrentProducerCursor)
        {
            return EMPMCQueueErrorStatus::BUFFER_EMPTY;
        }

        const int64 ClaimedIndex = ConsumerCursor.IncrementAndGetOldValue();
        
        /** Store the claimed element from the ring buffer in the Output var */
        Output = RingBuffer[CalculateIndex(ClaimedIndex)];
        
        return EMPMCQueueErrorStatus::TRANSACTION_SUCCESS;
    }
    
private:
    /**
     * @return Return the value of the @link IndexMask
     */
    FORCEINLINE int64 GetIndexMask() const noexcept
    {
        return IndexMask;
    }

    /**
     * @return Calculate an index for the ring buffer.
     * This avoids using modulo (%) in favour of a bitmask (&), which is faster.
     * For this to work the @link IndexMask MUST be a power of two minus one e.g 1023.
     */
    FORCEINLINE int64 CalculateIndex(const uint64 IndexValue) const noexcept
    {
        return IndexValue & GetIndexMask();
    }
    
private:
    MPMC_PADDING PadToAvoidContention0[PLATFORM_CACHE_LINE_SIZE] = { };
    /** Stores a value that MUST be one less than a power of two e.g 1023.
    * Used to calculate an index for access to the @link RingBuffer.
    */
    alignas(alignof(volatile int64) * 2) volatile int64        IndexMask; 
    MPMC_PADDING PadToAvoidContention1[PLATFORM_CACHE_LINE_SIZE] = { };
    /**
     * This is the pointer to the ring buffer which holds the queue's data.
     * This is allocated in the default constructor using calloc.
     */
    MPMC_ALIGNMENT FElementType*                               RingBuffer;
    MPMC_PADDING PadToAvoidContention2[PLATFORM_CACHE_LINE_SIZE] = { };
    /**
     * The cursor that holds the next available index on the ring buffer for Consumers.
     */
    MPMC_ALIGNMENT FCursor                                      ConsumerCursor;
    MPMC_PADDING PadToAvoidContention3[PLATFORM_CACHE_LINE_SIZE] = { };
    /**
     * The cursor that holds the next available index on the ring buffer for Producers.
     */
    MPMC_ALIGNMENT FCursor                                      ProducerCursor;
    MPMC_PADDING PadToAvoidContention4[PLATFORM_CACHE_LINE_SIZE] = { };
};

#endif // MPMCQUEUE_H
