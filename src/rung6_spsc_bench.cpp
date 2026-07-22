// Rung 6 — cross-core handoff latency over the SPSC ring.
//
// The queue is the substrate; the measurement is the deliverable. Two phases:
//
//   LATENCY (lockstep, exactly one item in flight): the producer stamps rdtsc
//   into a slot, pushes it, then waits until the consumer has popped it before
//   sending the next. The consumer records now - stamp. With one item in flight
//   the ring is always near-empty, so this measures the TRUE cross-core handoff
//   — the coherence transfer of the slot and the write_ index to the consumer's
//   core plus the consumer's load — not residence time in a buffered ring. This
//   is also the regime where the write_/read_ lines bounce cross-core every op,
//   so false sharing (the nopad build) actually shows up here.
//
//   THROUGHPUT (streaming): the producer blasts, the consumer drains up to
//   --batch per spin. Reports max sustained M msg/s and the latency/throughput
//   trade as batch grows.
//
// TSC is invariant + synced across cores on this single-socket part, so a
// cross-core cycle delta is meaningful (for the HT-sibling placement the two
// threads share the physical core's TSC trivially). Consumer hot-spins so we
// measure handoff, never a scheduler wakeup.
//
// Usage: rung6_spsc_bench --prod 6 --cons 7 [--n 2000000] [--batch 32]
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <vector>

#include "spsc_ring.hpp"
#include "tsc.h"
#include "percentiles.h"

#ifndef SPSC_PAD
#define SPSC_PAD 64
#endif

struct Slot { uint64_t tsc; uint64_t seq; };

static constexpr std::size_t kCap = 1u << 14;   // 16384 slots (for the throughput phase)
using Ring = SpscRing<Slot, kCap, SPSC_PAD>;

static bool pin(int core) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s);
    return pthread_setaffinity_np(pthread_self(), sizeof(s), &s) == 0;
}

int main(int argc, char** argv) {
    int prod_core = 6, cons_core = 7, batch = 32;
    uint64_t N = 2000000;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--prod") && i + 1 < argc) prod_core = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cons") && i + 1 < argc) cons_core = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--n") && i + 1 < argc) N = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--batch") && i + 1 < argc) batch = std::atoi(argv[++i]);
    }

    const double ns_per_cycle = calibrate_ns_per_cycle();
    const uint64_t overhead = timer_overhead_cycles();

    // -------------------- Phase 1: LATENCY (lockstep) -----------------------
    {
        static Ring ring;
        std::vector<uint64_t> lat; lat.reserve(N);
        std::atomic<bool> ready{false};
        std::atomic<uint64_t> popped{0};

        std::thread consumer([&] {
            if (!pin(cons_core)) std::fprintf(stderr, "warn: consumer pin failed\n");
            ready.store(true, std::memory_order_release);
            uint64_t got = 0; Slot s;
            while (got < N) {
                if (ring.try_pop(s)) {
                    const uint64_t now = now_cycles();
                    uint64_t d = now - s.tsc;
                    d = d > overhead ? d - overhead : 0;
                    lat.push_back(d);
                    ++got;
                    popped.store(got, std::memory_order_release);   // signal producer
                }
            }
        });

        while (!ready.load(std::memory_order_acquire)) { }
        if (!pin(prod_core)) std::fprintf(stderr, "warn: producer pin failed\n");
        for (uint64_t i = 0; i < N; ++i) {
            Slot s{now_cycles(), i};
            while (!ring.try_push(s)) { }
            while (popped.load(std::memory_order_acquire) != i + 1) { } // one in flight
        }
        consumer.join();

        for (auto& d : lat) d = static_cast<uint64_t>(d * ns_per_cycle + 0.5);
        char label[96];
        std::snprintf(label, sizeof(label), "handoff prod=%d cons=%d pad=%d",
                      prod_core, cons_core, SPSC_PAD);
        report(label, lat, "ns");
    }

    // -------------------- Phase 2: THROUGHPUT (streaming) -------------------
    {
        static Ring ring;
        std::atomic<bool> ready{false};
        uint64_t t_start = 0, t_end = 0;

        std::thread consumer([&] {
            if (!pin(cons_core)) { }
            ready.store(true, std::memory_order_release);
            uint64_t got = 0; Slot s;
            while (got < N) {
                int drained = 0;
                while (drained < batch && ring.try_pop(s)) { ++got; ++drained; do_not_optimize(&s); }
            }
            t_end = now_cycles();
        });
        while (!ready.load(std::memory_order_acquire)) { }
        if (!pin(prod_core)) { }
        t_start = now_cycles();
        for (uint64_t i = 0; i < N; ++i) {
            Slot s{i, i};
            while (!ring.try_push(s)) { }
        }
        consumer.join();

        const double elapsed_ns = (t_end - t_start) * ns_per_cycle;
        const double mps = N / (elapsed_ns / 1e9) / 1e6;
        std::fprintf(stderr, "%-28s throughput=%.1f M msg/s (batch=%d)  ns_per_cyc=%.4f\n",
                     "", mps, batch, ns_per_cycle);
    }
    return 0;
}
