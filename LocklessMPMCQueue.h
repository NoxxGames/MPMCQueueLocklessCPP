// SPDX-License-Identifier: GPL-2.0-or-later
/** Lockless Multi-Producer Multi-Consumer Queue Type.
 * Author: Primrose Taylor
 */

#ifndef LOCKLESS_MPMC_QUEUE
#define LOCKLESS_MPMC_QUEUE

#include <atomic>

#if defined(_MSC_VER)
    #define builtin_cpu_pause() _mm_pause();
#elif defined(__clang__) || defined(__GNUC__)
    #define builtin_cpu_pause() __builtin_ia32_pause();
#endif

template<typename T, uint_least32_t QUEUE_SIZE>
class lockless_mpmc_queue final
{
private:
    static constexpr uint_fast32_t cache_line_size = 64;
    
    struct alignas(cache_line_size) queue_data
    {
        const uint_least32_t index_mask;
        std::atomic<uint_least32_t> consumer_cursor;
        std::atomic<uint_least32_t> producer_cursor;
        T *ring_buffer;
        uint_least8_t padding_bytes[
            cache_line_size - sizeof(uint_least32_t) -
            sizeof(std::atomic<uint_least32_t>) -
            sizeof(std::atomic<uint_least32_t>) -
            sizeof(T*)
            % cache_line_size];

        queue_data(const uint_least32_t initial_index_mask)
            : index_mask(initial_index_mask),
            consumer_cursor(0),
            producer_cursor(0),
            ring_buffer(nullptr),
            padding_bytes{0}
        {
        }
    };
    
public:
    lockless_mpmc_queue()
        : data(ceil_to_nearest_power_of_two_minus_one())
    {
        static_assert(QUEUE_SIZE > 0, "Can't have a queue of size 0!");
        static_assert(QUEUE_SIZE <= 0xffffffff,
            "Can't have a queue size which is above 32 bits!");
        static_assert(std::is_copy_constructible_v<T> ||
            std::is_copy_assignable_v<T> ||
            std::is_move_assignable_v<T> || std::is_move_constructible_v<T>,
            "Can't use non-copyable, non-assignable, non-movable, or non-constructible type!");
        
        data.ring_buffer = (T*)calloc(
            data.index_mask + 1, sizeof(T));
    }
    
    ~lockless_mpmc_queue()
    {
        if(data.ring_buffer != nullptr)
        {
            free(data.ring_buffer);
        }
    }
    
    bool push(const T& new_element)
    {
        uint_least32_t current_producer_cursor;
        uint_least32_t new_producer_cursor;
        
        do
        {
            current_producer_cursor =
                data.producer_cursor.load(std::memory_order_acquire);
            const uint_least32_t current_consumer_cursor =
                data.consumer_cursor.load(std::memory_order_acquire);
            
            new_producer_cursor = current_producer_cursor + 1;

            // Check if the buffer is full
            if(new_producer_cursor == current_consumer_cursor)
            {
                return false;
            }
            
            builtin_cpu_pause();
        } while (!custom_cas(data.producer_cursor,
            current_producer_cursor,
             new_producer_cursor));

        data.ring_buffer[current_producer_cursor & data.index_mask] = new_element;
        
        return true; 
    }
    
    bool pop(T& element)
    {
        uint_least32_t current_consumer_cursor;
        uint_least32_t new_consumer_cursor;

        do
        {
            current_consumer_cursor =
                data.consumer_cursor.load(std::memory_order_acquire);
            const uint_least32_t current_producer_cursor = 
                data.producer_cursor.load(std::memory_order_acquire);

            // Check if the buffer is empty
            if(current_consumer_cursor == current_producer_cursor)
            {
                return false;
            }

            new_consumer_cursor = current_consumer_cursor + 1;
            
            builtin_cpu_pause();
        } while (!custom_cas(data.consumer_cursor,
            current_consumer_cursor,
            new_consumer_cursor));

        element = data.ring_buffer[current_consumer_cursor & data.index_mask];
        
        return true;
    }
    
    uint_least32_t size() const
    {
        const uint_fast32_t current_producer_cursor =
            data.producer_cursor.load(std::memory_order_acquire);
        const uint_fast32_t current_consumer_cursor =
            data.consumer_cursor.load(std::memory_order_acquire);

        return current_producer_cursor - current_consumer_cursor;
    }

    bool empty() const
    {
        return size() == 0;
    }

    bool full() const
    {
        return size() == (data.index_mask + 1);
    }
    
private:
    uint_least32_t ceil_to_nearest_power_of_two_minus_one() const
    {
        if(QUEUE_SIZE == 0)
        {
            return 0;
        }
        
        uint_least32_t nearest_power = QUEUE_SIZE;
        
        nearest_power--;
        nearest_power |= nearest_power >> 1; // 2 bit
        nearest_power |= nearest_power >> 2; // 4 bit
        nearest_power |= nearest_power >> 4; // 8 bit
        nearest_power |= nearest_power >> 8; // 16 bit
        nearest_power |= nearest_power >> 16; // 32 bit
        // nearest_power++; // skip this final step to get n^2 - 1
        
        return nearest_power;
    }

    static bool custom_cas(std::atomic<uint_least32_t>& atomic_var,
        const uint_least32_t expected, const uint_least32_t desired)
    {
        const uint_least32_t current_value =
            atomic_var.load(std::memory_order_relaxed);

        if(current_value == expected)
        {
            atomic_var.store(desired, std::memory_order_release);
            return true;
        }
        
        return false;
    }
    
private:
    queue_data data;

private:
    lockless_mpmc_queue(const lockless_mpmc_queue&) = delete;
    lockless_mpmc_queue& operator=(const lockless_mpmc_queue&) = delete;
};

#endif
