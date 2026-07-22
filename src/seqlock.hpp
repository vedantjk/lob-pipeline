#pragma once
// Single-writer / multi-reader seqlock. The writer never blocks on readers;
// readers are wait-free (they retry, never stalling the writer). This is the
// standard top-of-book publish primitive: the book-update thread writes, and
// strategy/logging/risk threads read snapshots without ever touching the hot
// path.
//
// Mechanism:
//   writer:  seq -> odd (release); write payload; seq -> even (release)
//   reader:  load seq (acquire); if odd, retry; read payload; re-load seq;
//            if it changed, retry (a write straddled our read → torn, discard).
//
// The subtlety that is the whole point: a naive seqlock stores the payload with
// PLAIN (non-atomic) stores while a reader may be reading it. That is a data
// race — undefined behavior under the C++ memory model — even though it happens
// to "work" on x86. The well-defined form (Boehm, "Can Seqlocks Get Along with
// Programming Language Memory Models?") stores the payload as RELAXED ATOMIC
// words bracketed by acquire/release fences against the sequence counter. This
// is correct under C++11, not merely x86-correct, and is TSan-clean.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

template <typename T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock payload must be trivially copyable");
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(uint64_t) - 1) / sizeof(uint64_t);

    alignas(64) std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> words_[kWords];   // payload as relaxed-atomic words (Boehm)

public:
    SeqLock() {
        for (std::size_t i = 0; i < kWords; ++i)
            words_[i].store(0, std::memory_order_relaxed);
    }

    // Writer only (single writer).
    void store(const T& v) noexcept {
        const uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);           // odd: update in progress
        std::atomic_thread_fence(std::memory_order_release);

        uint64_t tmp[kWords];
        std::memset(tmp, 0, sizeof(tmp));
        std::memcpy(tmp, &v, sizeof(T));
        for (std::size_t i = 0; i < kWords; ++i)
            words_[i].store(tmp[i], std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);           // even: done
    }

    // Reader (wait-free, any number of concurrent readers). Returns a consistent
    // snapshot; retries internally on a straddling write.
    T load() const noexcept {
        uint64_t tmp[kWords];
        uint64_t s0, s1;
        do {
            s0 = seq_.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_acquire);
            for (std::size_t i = 0; i < kWords; ++i)
                tmp[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            s1 = seq_.load(std::memory_order_acquire);
        } while (s0 != s1 || (s0 & 1));      // changed mid-read, or writer active → retry
        T out;
        std::memcpy(&out, tmp, sizeof(T));
        return out;
    }

    // Like load(), but reports whether any retry occurred (for retry-rate stats).
    T load_counting(uint64_t& retries) const noexcept {
        uint64_t tmp[kWords];
        uint64_t s0, s1;
        for (;;) {
            s0 = seq_.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_acquire);
            for (std::size_t i = 0; i < kWords; ++i)
                tmp[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            s1 = seq_.load(std::memory_order_acquire);
            if (s0 == s1 && !(s0 & 1)) break;
            ++retries;
        }
        T out;
        std::memcpy(&out, tmp, sizeof(T));
        return out;
    }

    uint64_t seq() const noexcept { return seq_.load(std::memory_order_acquire); }
};
