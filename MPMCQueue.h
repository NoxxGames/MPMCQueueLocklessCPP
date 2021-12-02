#ifndef MPMCQUEUE_H
#define MPMCQUEUE_H

#include "stdio.h"
#include "stdlib.h"

#include <atomic>
#include <vector>

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

template <typename T>
class MPMC_ALIGNMENT TSequentialContainer
{
public:
    explicit TSequentialContainer()
    {
        static_assert(
            std::is_copy_constructible_v<T>	    ||
            std::is_copy_assignable_v<T>		||
            std::is_move_assignable_v<T>		||
            std::is_move_constructible_v<T>,
            "Can't use non-copyable, non-assignable, non-movable, or non-constructible type!"
        );
    }

    FORCEINLINE T Get() const
    {
        const T OutCopy = Data.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        return OutCopy;
    }

    FORCEINLINE void Set(const T& NewData)
    {
        std::atomic_thread_fence(std::memory_order_release);
        Data.store(NewData, std::memory_order_relaxed);
    }

    FORCEINLINE void SetVolatile(const T& NewData)
    {
        std::atomic_thread_fence(std::memory_order_release);
        Data.store(NewData, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    FORCEINLINE bool CompareAndSet(T& Expected, const T& NewValue)
    {
        return Data.compare_exchange_strong(Expected, NewValue,
            std::memory_order_release, std::memory_order_relaxed);
    }
    
protected:
    MPMC_PADDING PadToAvoidContention0[PLATFORM_CACHE_LINE_SIZE] = { };
    std::atomic<T> Data;
    MPMC_PADDING PadToAvoidContention1[PLATFORM_CACHE_LINE_SIZE] = { };
};

class MPMC_ALIGNMENT FSequentialInteger : public TSequentialContainer<int64>
{
public:
    explicit FSequentialInteger(const int64 InitialValue = 0)
        : TSequentialContainer()
    {
        Data.store(InitialValue, std::memory_order_relaxed);
    }

    FORCEINLINE int64 AddAndGetOldValue(const int64 Value)
    {
        return Data.fetch_add(Value, std::memory_order_acq_rel);
    }

    FORCEINLINE int64 AddAndGetNewValue(const int64 Value)
    {
        return AddAndGetOldValue(Value) + Value;
    }

    FORCEINLINE int64 IncrementAndGetOldValue()
    {
        return AddAndGetOldValue(1);
    }
};

template <typename T, uint64 TQueueSize = 1024>
class TMPMCQueue final : public FNoncopyable
{
private:
    using FElementType = T;
    
public:
    explicit TMPMCQueue()
    {
        RingBuffer = MallocZeroed();
        
        ConsumerCursor.SetVolatile(0);
        ProducerCursor.SetVolatile(0);
    }
    
    bool Enqueue(const FElementType& NewElement)
    {
        const int64 CurrentConsumerCursor = ConsumerCursor.Get();
        const int64 CurrentProducerCursor = ProducerCursor.Get();
        
        /** Return false if the buffer is full */
        if((CurrentProducerCursor + 1) == CurrentConsumerCursor)
        {
            return false;
        }
        
        int64 ClaimedIndex = CurrentProducerCursor;
        int64 NewIndex = ClaimedIndex + 1;
        
        while(!ProducerCursor.CompareAndSet(ClaimedIndex, NewIndex)) // Spin wait for cursor to update
        {
            ClaimedIndex = ProducerCursor.Get();
            NewIndex = ClaimedIndex + 1;
        }
        
        /** Update the index on the ring buffer with the new element */
        RingBuffer[CalculateIndex(ClaimedIndex)] = NewElement;
        
        return true;
    }

    bool Dequeue(FElementType& Output)
    {
        const int64 CurrentConsumerCursor = ConsumerCursor.Get();
        const int64 CurrentProducerCursor = ProducerCursor.Get();

        if(CurrentConsumerCursor == CurrentProducerCursor)
        {
            return false;
        }

        int64 ClaimedIndex = CurrentConsumerCursor;
        int64 NewIndex = ClaimedIndex + 1;

        while(!ConsumerCursor.CompareAndSet(ClaimedIndex, NewIndex)) // Spin wait for cursor to update
        {
            ClaimedIndex = ConsumerCursor.Get();
            NewIndex = ClaimedIndex + 1;
        }

        /** Store the claimed element from the ring buffer in the Output var */
        Output = RingBuffer[CalculateIndex(ClaimedIndex)];
        
        return true;
    }
    
private:
    FORCEINLINE int64 GetIndexMask() const noexcept
    {
        return IndexMask;
    }

    FORCEINLINE int64 CalculateIndex(const uint64 IndexValue) const noexcept
    {
        return IndexValue & GetIndexMask();
    }

    /** Contiguous memory allocation. All elements initialised to zero. */
    FORCEINLINE FElementType* MallocZeroed()
    {
        return (FElementType*)calloc(TQueueSize, sizeof(FElementType));
    }
    
private:
    MPMC_PADDING PadToAvoidContention0[PLATFORM_CACHE_LINE_SIZE] = { };
    alignas(alignof(const volatile int64) * 2) const volatile int64 IndexMask = TQueueSize - 1; // not a clue TODO:
    MPMC_PADDING PadToAvoidContention1[PLATFORM_CACHE_LINE_SIZE] = { };
    MPMC_ALIGNMENT FElementType*                                RingBuffer;
    MPMC_PADDING PadToAvoidContention2[PLATFORM_CACHE_LINE_SIZE] = { };
    MPMC_ALIGNMENT FSequentialInteger                           ConsumerCursor;
    MPMC_PADDING PadToAvoidContention3[PLATFORM_CACHE_LINE_SIZE] = { };
    MPMC_ALIGNMENT FSequentialInteger                           ProducerCursor;
    MPMC_PADDING PadToAvoidContention4[PLATFORM_CACHE_LINE_SIZE] = { };
};

#endif // MPMCQUEUE_H
