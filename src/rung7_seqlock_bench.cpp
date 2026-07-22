// Rung 7 — seqlock top-of-book publisher: reader-retry rate vs writer frequency.
//
// One writer thread republishes a snapshot at a controllable rate (a spin knob
// between updates); one reader thread hot-loops load_counting() and tallies
// retries. Two things come out:
//   1. Torn-read invariant: the snapshot carries a checksum field; a reader that
//      ever observes checksum != f(fields) saw a torn snapshot. It must never
//      fire — that is the correctness claim, checked under a hot writer.
//   2. Retry rate = retries / successful-loads, swept across writer spin gaps.
//      As the writer approaches the reader's load latency, retries climb; a
//      reader can in principle livelock if the writer always outruns it. We show
//      the curve and the bound.
//
// Usage: rung7_seqlock_bench --wcore 6 --rcore 7 [--secs 2]
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <thread>

#include "seqlock.hpp"
#include "tsc.h"

// A top-of-book-shaped snapshot with a redundant checksum so torn reads are
// detectable. Kept > 1 cache line so a torn read is actually reachable.
struct ToBSnap {
    uint64_t version;
    uint64_t bid_px, bid_qty, ask_px, ask_qty;
    uint64_t pad[3];
    uint64_t checksum;   // FNV-1a over the data words (see csum)
};
// FNV-1a, not XOR: XOR has a large null space (a two-word tear whose deltas
// cancel passes undetected). A multiplicative mix has no such coincidental
// cancellation for the field values we generate, so a torn snapshot is caught.
static inline uint64_t csum(const ToBSnap& s) {
    uint64_t h = 1469598103934665603ULL;
    for (uint64_t x : {s.version, s.bid_px, s.bid_qty, s.ask_px, s.ask_qty})
        h = (h ^ x) * 1099511628211ULL;
    return h;
}

static bool pin(int core) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s);
    return pthread_setaffinity_np(pthread_self(), sizeof(s), &s) == 0;
}

static SeqLock<ToBSnap> g_lock;

int main(int argc, char** argv) {
    int wcore = 6, rcore = 7; double secs = 2.0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--wcore") && i + 1 < argc) wcore = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--rcore") && i + 1 < argc) rcore = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--secs") && i + 1 < argc) secs = std::atof(argv[++i]);
    }
    const double ns_per_cycle = calibrate_ns_per_cycle();
    const uint64_t run_cycles = static_cast<uint64_t>(secs * 1e9 / ns_per_cycle);

    // Writer spin-gaps to sweep (cycles of busy work between publishes). 0 = as
    // fast as possible (worst case for the reader).
    const uint64_t gaps[] = {0, 50, 200, 1000, 5000};
    const int NG = sizeof(gaps) / sizeof(gaps[0]);

    std::fprintf(stderr, "seqlock wcore=%d rcore=%d  payload=%zuB (%zu words)\n",
                 wcore, rcore, sizeof(ToBSnap), (sizeof(ToBSnap) + 7) / 8);
    std::fprintf(stderr, "%-12s %-14s %-14s %-14s %-10s\n",
                 "writer_gap", "writes/s(M)", "loads/s(M)", "retries/s(M)", "retry_frac");

    // Seed a checksum-valid snapshot so a reader can never catch the pristine
    // pre-write state (zero payload with a zero checksum field != csum(0..0)) and
    // mistake it for a tear. seq is monotonic, so one seed covers all iterations.
    { ToBSnap init{}; init.checksum = csum(init); g_lock.store(init); }

    std::atomic<bool> torn{false};
    for (int gi = 0; gi < NG; ++gi) {
        const uint64_t gap = gaps[gi];
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> n_writes{0}, n_loads{0}, n_retries{0};
        uint64_t w_span = 0, r_span = 0;   // measured active cycles per thread

        std::thread writer([&] {
            if (!pin(wcore)) std::fprintf(stderr, "warn: writer pin failed\n");
            ToBSnap s{}; uint64_t v = 0;
            const uint64_t t0 = now_cycles();
            while (!stop.load(std::memory_order_relaxed)) {
                s.version = ++v;
                // Full-width, distinct, non-cancelling field values so a mid-struct
                // tear can't coincidentally reproduce the checksum.
                s.bid_px = 0x1000000000000000ULL + v * 0x9E3779B97F4A7C15ULL;
                s.bid_qty = v * 3 + 0xABCDu;
                s.ask_px = 0x2000000000000000ULL + v * 0x2545F4914F6CDD1DULL;
                s.ask_qty = ~v;
                s.checksum = csum(s);
                g_lock.store(s);
                n_writes.fetch_add(1, std::memory_order_relaxed);
                for (uint64_t k = 0; k < gap; k = k + 1) { asm volatile("" ::: "memory"); }
            }
            w_span = now_cycles() - t0;
        });

        std::thread reader([&] {
            if (!pin(rcore)) std::fprintf(stderr, "warn: reader pin failed\n");
            uint64_t loads = 0, retries = 0;
            const uint64_t t0 = now_cycles();
            while (now_cycles() - t0 < run_cycles) {
                uint64_t r = 0;
                ToBSnap s = g_lock.load_counting(r);
                if (csum(s) != s.checksum) torn.store(true, std::memory_order_relaxed);
                retries += r;
                ++loads;
            }
            r_span = now_cycles() - t0;
            n_loads.store(loads); n_retries.store(retries);
            stop.store(true, std::memory_order_relaxed);
        });

        writer.join(); reader.join();

        // Rates use each thread's OWN measured active span, not the nominal secs
        // (the writer runs slightly past `secs` until it observes stop).
        const double w_secs = w_span * ns_per_cycle / 1e9;
        const double r_secs = r_span * ns_per_cycle / 1e9;
        const double wl = n_writes.load(), ld = n_loads.load(), rt = n_retries.load();
        std::fprintf(stderr, "%-12llu %-14.2f %-14.2f %-14.2f %-10.4f\n",
                     (unsigned long long)gap,
                     w_secs > 0 ? wl / w_secs / 1e6 : 0.0,
                     r_secs > 0 ? ld / r_secs / 1e6 : 0.0,
                     r_secs > 0 ? rt / r_secs / 1e6 : 0.0,
                     ld > 0 ? rt / ld : 0.0);
    }

    if (torn.load()) { std::fprintf(stderr, "FAIL: torn snapshot observed\n"); return 1; }
    std::fprintf(stderr, "PASS: no torn snapshot across all writer rates\n");
    return 0;
}
