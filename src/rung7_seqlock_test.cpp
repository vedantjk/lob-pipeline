// Rung 7 correctness — seqlock under TSan (see rung7_seqlock_test target).
//
// One writer, several readers, all hammering concurrently. Two claims:
//   1. No torn read: every reader-observed snapshot satisfies its checksum.
//   2. No data race: because the payload is relaxed-atomic words bracketed by
//      fences (the Boehm form), TSan reports CLEAN. The naive plain-store
//      seqlock would trip TSan here even though it "works" on x86 — that is the
//      whole reason for the relaxed-atomic payload.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <thread>
#include <vector>

#include "seqlock.hpp"

struct Snap {
    uint64_t v, a, b, c, d;
    uint64_t pad[3];
    uint64_t checksum;
};
// FNV-1a rather than XOR: XOR can miss a two-word tear whose deltas cancel.
static inline uint64_t csum(const Snap& s) {
    uint64_t h = 1469598103934665603ULL;
    for (uint64_t x : {s.v, s.a, s.b, s.c, s.d}) h = (h ^ x) * 1099511628211ULL;
    return h;
}

static SeqLock<Snap> g;

int main() {
    constexpr int kReaders = 4;
    constexpr uint64_t kWrites = 5'000'000;
    std::atomic<bool> stop{false};
    std::atomic<bool> torn{false};
    std::atomic<uint64_t> total_loads{0};

    // Publish a checksum-valid snapshot BEFORE readers start. Otherwise a reader
    // can observe the pristine pre-write state (seq=0, all-zero payload) — a
    // perfectly consistent snapshot, but its zero checksum field != csum(0..0),
    // which would read as a false tear. seq only increases, so once we've stored
    // this, that pristine state is never observable again.
    { Snap init{}; init.checksum = csum(init); g.store(init); }

    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i)
        readers.emplace_back([&] {
            uint64_t loads = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                Snap s = g.load();
                if (csum(s) != s.checksum) torn.store(true, std::memory_order_relaxed);
                ++loads;
            }
            total_loads.fetch_add(loads, std::memory_order_relaxed);
        });

    std::thread writer([&] {
        Snap s{};
        for (uint64_t v = 1; v <= kWrites; ++v) {
            s.v = v; s.a = v * 3; s.b = v ^ 0xAAAA; s.c = ~v; s.d = v << 1;
            s.checksum = csum(s);
            g.store(s);
        }
        stop.store(true, std::memory_order_relaxed);
    });

    writer.join();
    for (auto& r : readers) r.join();

    std::fprintf(stderr, "writes=%llu readers=%d total_loads=%llu\n",
                 (unsigned long long)kWrites, kReaders, (unsigned long long)total_loads.load());
    if (torn.load()) { std::fprintf(stderr, "FAIL: torn snapshot observed\n"); return 1; }
    std::fprintf(stderr, "PASS: no torn snapshot; run under TSan to confirm race-free\n");
    return 0;
}
