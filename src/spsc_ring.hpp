#pragma once
// Bounded single-producer / single-consumer lock-free ring.
//
// Power-of-two capacity, mask wraparound, preallocated, zero hot-path
// allocation (same constraint as the order-book pools). Indices are monotonic
// size_t (never wrapped); we mask only when indexing the storage. Full when
// write - read == Capacity; empty when write == read.
//
// The memory ordering IS the point:
//   producer:  buf_[w] = v;  write_.store(w+1, release);
//   consumer:  write_.load(acquire) synchronizes-with that release, so the slot
//              write is visible; then read_.store(r+1, release) frees the slot.
// On x86-TSO load-acquire / store-release are plain movs — a compiler fence
// only, zero hardware cost. seq_cst would force a store-load barrier (mfence or
// a locked instruction) on the producer's store: a real, measurable tax.
//
// Optimization: each side caches the opposite index and only re-reads the
// shared (cross-core) line when the ring *appears* full/empty. This keeps a
// cross-core cache-line read off the common-case hot path (Disruptor/Vyukov).
//
// Pad controls false-sharing isolation. Pad=64 gives write_ and read_ their own
// cache lines. Pad=1 co-locates them on one line so producer and consumer
// ping-pong the same line every op — the deliberately-wrong layout we measure.

#include <atomic>
#include <cstddef>

template <typename T, std::size_t Capacity, std::size_t Pad = 64>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static constexpr std::size_t kMask = Capacity - 1;

    // Shared, written by one side and read cross-core by the other.
    alignas(Pad) std::atomic<std::size_t> write_{0};
    alignas(Pad) std::atomic<std::size_t> read_{0};

    // Private: producer's cached view of read_, consumer's cached view of write_.
    alignas(Pad) std::size_t read_cache_{0};
    alignas(Pad) std::size_t write_cache_{0};

    alignas(64) T buf_[Capacity];

public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer only.
    bool try_push(const T& v) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);   // we own write_
        if (w - read_cache_ == Capacity) {                              // maybe full
            read_cache_ = read_.load(std::memory_order_acquire);
            if (w - read_cache_ == Capacity) return false;             // really full
        }
        buf_[w & kMask] = v;
        write_.store(w + 1, std::memory_order_release);                // publishes the slot
        return true;
    }

    // Consumer only.
    bool try_pop(T& out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);    // we own read_
        if (r == write_cache_) {                                        // maybe empty
            write_cache_ = write_.load(std::memory_order_acquire);
            if (r == write_cache_) return false;                       // really empty
        }
        out = buf_[r & kMask];
        read_.store(r + 1, std::memory_order_release);                  // frees the slot
        return true;
    }

    // Observers (approximate; for tests/telemetry, not hot-path control flow).
    std::size_t size_approx() const noexcept {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }
    static constexpr std::size_t capacity() noexcept { return Capacity; }
};
