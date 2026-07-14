#pragma once
#include <cstdint>
#include <x86intrin.h>

inline uint64_t now_cycles()
{
    unsigned aux;
    const uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

inline uint32_t current_core()
{
    unsigned aux;
    __rdtscp(&aux);
    return aux & 0xFFF;
}

inline void do_not_optimize(void* p)
{
    asm volatile("" : : "g"(p) : "memory");
}

double calibrate_ns_per_cycle();
uint64_t timer_overhead_cycles();
