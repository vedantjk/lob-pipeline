#pragma once
#include <cstdint>

constexpr std::size_t MAX_PAYLOAD = 64;   // largest framed ITCH message we accept
constexpr uint32_t PRICE_SCALE = 10000;   // raw price -> USD divisor
constexpr uint64_t NS_PER_SEC = 1000000000ULL;

// Header fields, present on every message (ITCH 5.0 layout).
constexpr int STOCK_LOCATE_OFFSET = 1;
constexpr int TIMESTAMP_OFFSET = 5;        // 48-bit ns since midnight

// Add Order (A) / Add Order w/ MPID (F): both share the relevant prefix layout.
constexpr int ADD_REF_OFFSET = 11;
constexpr int ADD_SIDE_OFFSET = 19;        // 'B' = buy, else sell
constexpr int ADD_SHARES_OFFSET = 20;
constexpr int ADD_PRICE_OFFSET = 32;

// Order Executed (E) / Executed w/ Price (C) / Cancel (X).
constexpr int EXEC_REF_OFFSET = 11;
constexpr int EXEC_SHARES_OFFSET = 19;

// Order Delete (D).
constexpr int DELETE_REF_OFFSET = 11;

// Order Replace (U).
constexpr int U_ORIGREF_OFFSET = 11;
constexpr int U_NEWREF_OFFSET = 19;
constexpr int U_SHARES_OFFSET = 27;
constexpr int U_PRICE_OFFSET = 31;

// System Event (S): event code char.
constexpr int S_EVENTCODE_OFFSET = 11;

// Stock Directory (R): 8-char symbol.
constexpr int R_SYMBOL_OFFSET = 11;
constexpr int R_SYMBOL_LEN = 8;

inline uint64_t msg_index = 0;

// Read an N-byte big-endian unsigned integer from p. Replaces the family of
// per-width swap helpers; narrow the result at the call site as needed.
template <std::size_t N>
uint64_t read_be(const uint8_t* p)
{
    uint64_t result = 0;
    for (std::size_t i = 0; i < N; ++i)
    {
        result = (result << 8) | p[i];
    }
    return result;
}

struct DecodedMsg
{
    char type;
    uint16_t locate;
    uint64_t timestamp;
    uint64_t order_id;
    uint8_t side;
    uint32_t price;
    uint32_t shares;
    uint64_t new_order_id;
};

inline DecodedMsg parse_add(const char type, const uint16_t locate, const uint64_t timestamp, const uint8_t* payload)
{
    const auto order_ref_number = read_be<8>(payload + ADD_REF_OFFSET);
    const auto shares = static_cast<uint32_t>(read_be<4>(payload + ADD_SHARES_OFFSET));
    const auto raw_price = static_cast<uint32_t>(read_be<4>(payload + ADD_PRICE_OFFSET));
    const uint8_t side = payload[ADD_SIDE_OFFSET] == 'B' ? 1 : 0;
    return {.type = type, .locate = locate, .timestamp = timestamp, .order_id = order_ref_number,  .side = side,. price = raw_price, .shares = shares};
}

inline DecodedMsg parse_execute(const char type, const uint16_t locate, const uint64_t timestamp, const uint8_t* payload)
{
    const auto order_ref_number = read_be<8>(payload + EXEC_REF_OFFSET);
    const auto executed = static_cast<uint32_t>(read_be<4>(payload + EXEC_SHARES_OFFSET));

    return {.type = type, .locate = locate, .timestamp = timestamp, .order_id = order_ref_number, .shares = executed};
}

inline DecodedMsg parse_delete(const char type, const uint16_t locate, const uint64_t timestamp, const uint8_t* payload)
{
    const auto order_ref_number = read_be<8>(payload + DELETE_REF_OFFSET);
    return {.type = type, .locate = locate, .timestamp = timestamp, .order_id = order_ref_number};
}

inline DecodedMsg parse_replace(const char type, const uint16_t locate, const uint64_t timestamp, const uint8_t* payload)
{
    const auto original_ref_number = read_be<8>(payload + U_ORIGREF_OFFSET);
    const auto new_ref_number = read_be<8>(payload + U_NEWREF_OFFSET);
    const auto new_shares = static_cast<uint32_t>(read_be<4>(payload + U_SHARES_OFFSET));
    const auto new_price = static_cast<uint32_t>(read_be<4>(payload + U_PRICE_OFFSET));

    return {.type = type, .locate = locate, .timestamp = timestamp, .order_id = original_ref_number, .price = new_price, .shares = new_shares, .new_order_id = new_ref_number};
}

inline DecodedMsg parser(const uint8_t* payload)
{
    const char type = static_cast<char>(payload[0]);
    const auto locate = static_cast<uint16_t>(read_be<2>(payload + STOCK_LOCATE_OFFSET));
    const auto timestamp = read_be<6>(payload + TIMESTAMP_OFFSET);

    switch (type)
    {
    case 'A':
    case 'F': return parse_add(type, locate, timestamp, payload);
    case 'E':
    case 'C':
    case 'X': return parse_execute(type, locate, timestamp, payload);
    case 'D': return parse_delete(type, locate, timestamp, payload);
    case 'U': return parse_replace(type, locate, timestamp, payload);
    default: return {};
    }
}