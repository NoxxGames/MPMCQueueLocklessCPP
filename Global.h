#ifndef GLOBAL_H
#define GLOBAL_H

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

template<typename T> using TMPMCAtomic = std::atomic<T>;

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

#endif // GLOBAL_HS
