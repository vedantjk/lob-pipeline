#pragma once
#include <cstdint>
#include <cstring>
#include "parser.h"   // read_be<N>

// Shared wire constants — single source of truth for sender and receiver.
constexpr uint16_t MOLD_PORT = 27502;
constexpr char MOLD_SESSION[10] = {'R','U','N','G','2','S','E','S','S','N'};
constexpr uint64_t MOLD_FIRST_SEQ = 1;      // seq of the first message of the session
constexpr std::size_t MOLD_HEADER_SIZE = 20; // 10B session + 8B seq + 2B count
constexpr std::size_t MOLD_MAX_PACKET = 1400; // stay under Ethernet MTU with headroom

// Write an N-byte big-endian unsigned integer to p (mirror of read_be).
template <std::size_t N>
void write_be(uint8_t* p, uint64_t v)
{
    for (std::size_t i = 0; i < N; ++i)
    {
        p[N - 1 - i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

struct MoldHeader
{
    char session[10];
    uint64_t seq;    // sequence number of the FIRST message block in this packet
    uint16_t count;  // 0 = heartbeat, 0xFFFF = end-of-session
};

inline void mold_pack_header(uint8_t* p, uint64_t seq, uint16_t count)
{
    std::memcpy(p, MOLD_SESSION, 10);
    write_be<8>(p + 10, seq);
    write_be<2>(p + 18, count);
}

inline MoldHeader mold_unpack_header(const uint8_t* p)
{
    MoldHeader h;
    std::memcpy(h.session, p, 10);
    h.seq = read_be<8>(p + 10);
    h.count = static_cast<uint16_t>(read_be<2>(p + 18));
    return h;
}

// Walk the N message blocks after the header: each is [u16 BE len][payload].
// Returns the payload pointer and advances *cursor past the block, or nullptr
// if the block would run past `end` (truncated/corrupt packet).
inline const uint8_t* mold_next_block(const uint8_t** cursor, const uint8_t* end, uint16_t* len_out)
{
    const uint8_t* p = *cursor;
    if (p + 2 > end) return nullptr;
    const auto len = static_cast<uint16_t>(read_be<2>(p));
    if (p + 2 + len > end) return nullptr;
    *len_out = len;
    *cursor = p + 2 + len;
    return p + 2;
}
