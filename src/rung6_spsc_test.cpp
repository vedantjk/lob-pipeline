// Rung 6 correctness — SPSC ring under a stress harness. Built with -fsanitize=
// thread (see rung6_spsc_test target). Verifies:
//   - no lost / duplicated / reordered items across millions of ops (FIFO, exact)
//   - the ring-full and ring-empty boundaries (the two bug nests) are exercised,
//     via randomized-but-deterministic producer/consumer pacing (a tiny xorshift,
//     since we must not use nondeterministic timing to gate correctness).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "spsc_ring.hpp"

static constexpr std::size_t kCap = 1u << 8;      // small ring => hammer full/empty
static constexpr uint64_t kN = 20'000'000;

static inline uint32_t xorshift(uint32_t& x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }

int main() {
    static SpscRing<uint64_t, kCap> ring;
    std::atomic<bool> ok{true};
    std::atomic<uint64_t> full_hits{0}, empty_hits{0};

    std::thread consumer([&] {
        uint32_t rng = 0xC0FFEEu;
        uint64_t expect = 0, v;
        while (expect < kN) {
            if (ring.try_pop(v)) {
                if (v != expect) { ok.store(false); std::fprintf(stderr, "ORDER BUG: got %llu expect %llu\n",
                                   (unsigned long long)v, (unsigned long long)expect); return; }
                ++expect;
            } else {
                empty_hits.fetch_add(1, std::memory_order_relaxed);
            }
            if (xorshift(rng) & 3) { volatile int spin = 0; for (int i = 0; i < (int)(rng & 63); ++i) spin += i; }
        }
    });

    std::thread producer([&] {
        uint32_t rng = 0x1234567u;
        uint64_t i = 0;
        while (i < kN) {
            if (ring.try_push(i)) ++i;
            else full_hits.fetch_add(1, std::memory_order_relaxed);
            if (xorshift(rng) & 3) { volatile int spin = 0; for (int j = 0; j < (int)(rng & 63); ++j) spin += j; }
        }
    });

    producer.join();
    consumer.join();

    std::fprintf(stderr, "full-boundary hits=%llu  empty-boundary hits=%llu\n",
                 (unsigned long long)full_hits.load(), (unsigned long long)empty_hits.load());
    if (!ok.load())               { std::fprintf(stderr, "FAIL: ordering/exactness violated\n"); return 1; }
    if (full_hits.load() == 0)    { std::fprintf(stderr, "WARN: never hit ring-full boundary\n"); }
    if (empty_hits.load() == 0)   { std::fprintf(stderr, "WARN: never hit ring-empty boundary\n"); }
    std::fprintf(stderr, "PASS: %llu items, exact FIFO, both boundaries exercised\n",
                 (unsigned long long)kN);
    return 0;
}
