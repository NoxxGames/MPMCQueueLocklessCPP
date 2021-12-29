#ifndef LOCKLESS_MPMC_QUEUE
#define LOCKLESS_MPMC_QUEUE

#include <atomic>
#include <thread>
#include <cassert> 

#if defined(_MSC_VER)
    #define builtin_cpu_pause() _mm_pause();
#elif defined(__clang__) || defined(__GNUC__)
    #define builtin_cpu_pause() __builtin_ia32_pause();
#endif

template<typename T, uint_least32_t queue_size_t>
class lockless_mpmc_queue final
{
private:
    static constexpr size_t cache_line_size = 64;
    
    struct alignas(cache_line_size) queue_data
    {
        const uint_least32_t index_mask;
        std::atomic<uint_least32_t> consumer_cursor;
        std::atomic<uint_least32_t> producer_cursor;
        T *ring_buffer;
        uint_least8_t padding_bytes[
            cache_line_size - sizeof(uint_least32_t) -
            sizeof(std::atomic<uint_least8_t>) - sizeof(std::atomic<uint_least8_t>) -
            sizeof(T*)
            % cache_line_size];

        queue_data(const uint_least32_t initial_index_mask) noexcept
            : index_mask(initial_index_mask),
            consumer_cursor(0),
            producer_cursor(0),
            ring_buffer(nullptr),
            padding_bytes{0}
        {
        }
    };
    
public:
    lockless_mpmc_queue() noexcept
        : data(ceil_to_nearest_power_of_two_minus_one())
    {
        assert(data.index_mask > 0);
        
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
    
    bool enqueue(const T& new_element)
    {
        uint_fast32_t current_producer_cursor;
        uint_fast32_t new_producer_cursor;
        
        do
        {
            current_producer_cursor = data.producer_cursor.load(std::memory_order_acquire);
            new_producer_cursor = current_producer_cursor + 1;
            const uint_fast32_t current_consumer_cursor =
                data.consumer_cursor.load(std::memory_order_acquire);

            if((new_producer_cursor) == current_consumer_cursor)
            {
                return false;
            }

            builtin_cpu_pause();
        } while (!data.producer_cursor.compare_exchange_weak(
            current_producer_cursor, new_producer_cursor,
            std::memory_order_acq_rel, std::memory_order_relaxed));

        data.ring_buffer[current_producer_cursor & data.index_mask] = new_element;
        
        return true;
    }

    bool batch_enqueue(const T* new_elements, const uint_fast32_t size)
    {
        if(!new_elements || size == 0)
        {
            return false;
        }

        uint_fast32_t current_producer_cursor;
        uint_fast32_t new_producer_cursor;
        
        do
        {
            current_producer_cursor = data.producer_cursor.load(std::memory_order_acquire);
            new_producer_cursor = current_producer_cursor + size;
            const uint_fast32_t current_consumer_cursor =
                data.consumer_cursor.load(std::memory_order_acquire);

            if(new_producer_cursor >= current_consumer_cursor)
            {
                return false;
            }
                
            builtin_cpu_pause();
        } while (!data.producer_cursor.compare_exchange_weak(
            current_producer_cursor, new_producer_cursor,
            std::memory_order_acq_rel, std::memory_order_relaxed));

        for(uint_least32_t i = 0; i < size; ++i)
        {
            if(!new_elements[i])
            {
                return false; // this is awkward because some prior valid elements may have already been added
            }
            
            data.ring_buffer[(current_producer_cursor + i) & data.index_mask] = new_elements[i];
        }
        
        return true;
    }
    
    bool dequeue(T& element)
    {
        uint_fast32_t current_consumer_cursor;
        uint_fast32_t new_consumer_cursor;
        
        do
        {
            current_consumer_cursor = data.consumer_cursor.load(std::memory_order_acquire);
            new_consumer_cursor = current_consumer_cursor + 1;
            const uint_fast32_t current_producer_cursor =
                data.producer_cursor.load(std::memory_order_acquire);

            if(current_consumer_cursor == current_producer_cursor)
            {
                return false;
            }

            builtin_cpu_pause();
        } while (!data.consumer_cursor.compare_exchange_weak(
            current_consumer_cursor, new_consumer_cursor,
            std::memory_order_acq_rel, std::memory_order_relaxed));

        element = data.ring_buffer[current_consumer_cursor & data.index_mask];
        
        return true;
    }
    
    bool batch_dequeue()
    {
        return true;
    }
    
private:
    uint_least32_t ceil_to_nearest_power_of_two_minus_one() const
    {
        if(queue_size_t == 0)
        {
            return 0;
        }
        
        uint_least32_t nearest_power = queue_size_t;
        
        nearest_power--;
        nearest_power |= nearest_power >> 1; // 2 bit
        nearest_power |= nearest_power >> 2; // 4 bit
        nearest_power |= nearest_power >> 4; // 8 bit
        nearest_power |= nearest_power >> 8; // 16 bit
        nearest_power |= nearest_power >> 16; // 32 bit

        return nearest_power;
    }
    
private:
    alignas(cache_line_size) queue_data data;
};

#endif
