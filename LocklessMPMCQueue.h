// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * C++14 32bit Lockless Bounded Circular MPMC Queue type.
 * Author: Primrose Taylor
 */

#ifndef BOUNDED_CIRCULAR_MPMC_QUEUE_H
#define BOUNDED_CIRCULAR_MPMC_QUEUE_H

#include "stdio.h"
#include "stdlib.h"

#include <atomic>
#include <stdint.h>
#include <functional>
#include <thread>

#define CACHE_LINE_SIZE     64U

#if defined(_MSC_VER)
    #define HARDWARE_PAUSE()                _mm_pause();
    #define _ENABLE_ATOMIC_ALIGNMENT_FIX    1 // MSVC atomic alignment fix.
    #define ATOMIC_ALIGNMENT                4
#else
    #define ATOMIC_ALIGNMENT                16
    #if defined(__clang__) || defined(__GNUC__)
        #define HARDWARE_PAUSE()            __builtin_ia32_pause();
    #endif
#endif

/**
 * Lockless, Multi-Producer, Multi-Consumer, Bounded Circular Queue type.
 * The type is intended to be light weight & portable.
 * The sub-types are all padded to fit within cache lines. Padding may be put
 * inbetween member variables if the variables are accessed seperatley.
 */
template <typename T, uint_least32_t queue_size, bool should_yield_not_pause = false>
class bounded_circular_mpmc_queue final
{
    /**
     * Simple, efficient spin-lock implementation.
     * A function that takes a void lambda function can be used to
     * conveiniently do something which will be protected by the lock.
     * @cite Credit to Erik Rigtorp https://rigtorp.se/spinlock/
     */
    class spin_lock
    {
        std::atomic<bool> lock_flag;
        
    public:
        spin_lock()
            : lock_flag{false}
        {
        }

        void do_work_through_lock(const std::function<void()> functor)
        {
            lock();
            functor();
            unlock();
        }
        
        void lock()
        {
            while (true)
            {
                if (!lock_flag.exchange(true, std::memory_order_acquire))
                {
                    break;
                }

                while (lock_flag.load(std::memory_order_relaxed))
                {
                    should_yield_not_pause ? std::this_thread::yield() : HARDWARE_PAUSE();
                }
            }
        }

        void unlock()
        {
            lock_flag.store(false, std::memory_order_release);
        }
    };

    /**
     * Structure that holds the two cursors.
     * The cursors are held together because we'll only ever be accessing
     * them both at the same time.
     * We don't directly align the struct because we need to use it as an
     * atomic variable, so we must align the atomic variable instead.
     */
    struct cursor_data
    {
        uint_fast32_t producer_cursor;
        uint_fast32_t consumer_cursor;
        uint8_t padding_bytes[CACHE_LINE_SIZE -
            sizeof(uint_fast32_t) -
            sizeof(uint_fast32_t)
            % CACHE_LINE_SIZE];

        cursor_data(const uint_fast32_t in_producer_cursor = 0,
            const uint_fast32_t in_consumer_cursor = 0)
            : producer_cursor(in_producer_cursor),
            consumer_cursor(in_consumer_cursor),
            padding_bytes{0}
        {
        }
    };

    /**
     * Structure that represents each node in the circular buffer.
     * Access to the data is protected by a spin lock.
     * Contention on the spin lock should be minimal, as it's only there
     * to prevent the case where a producer/consumer may try work with an element before
     * someone else has finished working with it. The data and the spin lock are seperated by
     * padding to put them in differnet cache lines, since they are not accessed
     * together in the case mentioned previously. The problem with this is
     * that in low contention cases, they will be accessed together, and thus
     * should be in the same cache line.
     */
    struct buffer_node
    {
        T data;
        uint8_t padding_bytes_0[CACHE_LINE_SIZE -
            sizeof(T) % CACHE_LINE_SIZE];
        spin_lock spin_lock_;
        uint8_t padding_bytes_1[CACHE_LINE_SIZE -
            sizeof(spin_lock)
            % CACHE_LINE_SIZE];

        buffer_node()
            : spin_lock_(),
            padding_bytes_0{0},
            padding_bytes_1{0}
        {
        }

        void get_data(T& out_data) const
        {
            spin_lock_.do_work_through_lock([&]()
            {
                out_data = data;
            });
        }

        void set_data(const T& in_data)
        {
            spin_lock_.do_work_through_lock([&]()
            {
               data = in_data; 
            });
        }
    };

    /**
     * Strucutre that contains the index mask, and the circular buffer.
     * Both are accessed at the same time, so they are not seperated by padding.
     */
    struct alignas(CACHE_LINE_SIZE) circular_buffer_data
    {
        const uint_fast32_t index_mask;
        buffer_node* circular_buffer;
        uint8_t padding_bytes[CACHE_LINE_SIZE -
            sizeof(const uint_fast32_t) -
            sizeof(buffer_node*)
            % CACHE_LINE_SIZE];

        circular_buffer_data()
            : index_mask(get_next_power_of_two()),
            padding_bytes{0}
        {
            static_assert(queue_size > 0, "Can't have a queue size <= 0!");
            static_assert(queue_size <= 0xffffffffU,
                "Can't have a queue length above 32bits!");

            /** Contigiously allocate the buffer.
              * The theory behind using calloc and not aligned_alloc
              * or equivelant, is that the memory should still be aligned,
              * since calloc will align by the type size, which in this case
              * is a multiple of the cache line size.
             */
            circular_buffer = (buffer_node*)calloc(
                index_mask + 1, sizeof(buffer_node));
        }

        ~circular_buffer_data()
        {
            if(circular_buffer != nullptr)
            {
                free(circular_buffer);
            }
        }
        
    private:
        /**
         * @cite https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
         */
        uint_least32_t get_next_power_of_two()
        {
            uint_least32_t v = queue_size;

            v--;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            v++;
            
            return v;
        }
    };
    
public:
    bounded_circular_mpmc_queue()
        : cursor_data_(cursor_data{}),
        circular_buffer_data_()
    {
    }

    /**
     * Push an element into the queue.
     * 
     * @param in_data Reference to the variable containg the data to be pushed.
     * @returns Returns false only if the buffer is full.
     */
    bool push(const T& in_data)
    {
        cursor_data current_cursor_data;

        // An infinite while-loop is used instead of a do-while, to avoid
        // the yield/pause happening before the CAS operation.
        while(true)
        {
            current_cursor_data = cursor_data_.load(std::memory_order_acquire);

            // Check if the buffer is full..
            if (current_cursor_data.producer_cursor + 1 == current_cursor_data.consumer_cursor)
            {
                return false;
            }

            // CAS operation used to make sure the cursors have not been incremented
            // by another producer/consumer before we got to this point, and to then increment
            // the cursor by 1 if it hasn't been changed.
            if (cursor_data_.compare_exchange_weak(current_cursor_data,
            {current_cursor_data.producer_cursor + 1,
                current_cursor_data.consumer_cursor},
            std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }

            should_yield_not_pause ? std::this_thread::yield() : HARDWARE_PAUSE();
        }

        // Set the data
        circular_buffer_data_.circular_buffer[
            current_cursor_data.producer_cursor & circular_buffer_data_.index_mask
            ].set_data(in_data);
        
        return true;
    }

    /**
     * Pop an element from the queue.
     * 
     * @param out_data Reference to the variable that will store the popped element.
     * @returns Returns false only if the buffer is empty.
     */
    bool pop(T& out_data)
    {
        cursor_data current_cursor_data;

        while(true)
        {
            current_cursor_data = cursor_data_.load(std::memory_order_acquire);

            // empty check 
            if (current_cursor_data.consumer_cursor == current_cursor_data.producer_cursor)
            {
                return false;
            }

            if (cursor_data_.compare_exchange_weak(current_cursor_data,
            {current_cursor_data.producer_cursor,
                current_cursor_data.consumer_cursor + 1},
                std::memory_order_release, std::memory_order_relaxed))
            {
                break;
            }
            
            should_yield_not_pause ? std::this_thread::yield() : HARDWARE_PAUSE();
        }

        // get the data
        circular_buffer_data_.circular_buffer[
            current_cursor_data.consumer_cursor & circular_buffer_data_.index_mask
            ].get_data(out_data);
        
        return true;
    }
    
    /**
     * @note Calling this function will increase contention on the cursor data!
     * @returns How many elements are currently in the buffer.
     */
    uint_fast32_t size() const
    {
        const cursor_data cursors = cursor_data_.load(std::memory_order_acquire);
        return cursors.producer_cursor - cursors.consumer_cursor;
    }

    /**
     * @note Calling this function will increase contention on the cursor data!
     * @returns Whether or not the buffer is empty.
     */
    bool empty() const
    {
        return size() == 0;
    }

    /**
     * @note Calling this function will increase contention on the cursor data!
     * @returns Whether or not the buffer is full.
     */
    bool full() const
    {
        return size() == circular_buffer_data_.index_mask + 1;
    }
    
private:
    alignas(CACHE_LINE_SIZE) std::atomic<cursor_data> cursor_data_;
    circular_buffer_data circular_buffer_data_;
    
private:
    bounded_circular_mpmc_queue(
        const bounded_circular_mpmc_queue&) = delete;
    bounded_circular_mpmc_queue& operator=(
        const bounded_circular_mpmc_queue&) = delete;
};

#endif
