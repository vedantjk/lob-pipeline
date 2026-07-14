#include "tsc.h"
#include <ctime>
#include <cstdint>

static uint64_t ns_between(const timespec& a, const timespec& b)
{
    return static_cast<uint64_t>(b.tv_sec - a.tv_sec) * 1000000000ULL
         + static_cast<uint64_t>(b.tv_nsec - a.tv_nsec);
}

double calibrate_ns_per_cycle()
{
    for (int i = 0; i < 1000; ++i) now_cycles();

    timespec t0{}, t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const uint64_t c0 = now_cycles();

    const uint64_t target_ns = 200000000ULL;
    do { clock_gettime(CLOCK_MONOTONIC, &t1); }
    while (ns_between(t0, t1) < target_ns);

    const uint64_t c1 = now_cycles();
    return static_cast<double>(ns_between(t0, t1)) / static_cast<double>(c1 - c0);
}

uint64_t timer_overhead_cycles()
{
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 1000; ++i)
    {
        const uint64_t a = now_cycles();
        const uint64_t b = now_cycles();
        const uint64_t d = b - a;
        if (d < best) best = d;
    }
    return best;
}
