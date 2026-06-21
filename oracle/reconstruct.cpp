// NASDAQ ITCH 5.0 limit-order-book reconstructor ("oracle").
//
// Reads a framed ITCH 5.0 dump, replays the order-flow into per-symbol books,
// and emits a top-of-book regression trace for the diff symbol (AAPL) plus a
// stdout summary report. Prices are kept as raw integers (1/10000 USD).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// --- Domain constants -------------------------------------------------------

// stock_locate of the diff symbol. The .tob regression trace is emitted only
// for this symbol.
constexpr uint16_t AAPL = 13;

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

// --- Big-endian reader ------------------------------------------------------

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

// --- Order / Book types -----------------------------------------------------

struct Order
{
    uint16_t stock_locate;
    uint8_t side;        // 1 = buy (bid), 0 = sell (ask)
    uint32_t raw_price;
    uint32_t shares;
};

// A single symbol's resting book: price -> aggregate resting quantity per side.
struct Book
{
    std::map<uint32_t, uint32_t> bids, asks;

    std::map<uint32_t, uint32_t>& side_for(uint8_t side)
    {
        return side ? bids : asks;
    }

    void add(uint8_t side, uint32_t price, uint32_t qty)
    {
        side_for(side)[price] += qty;
    }

    // Subtract qty from the resting level at price; erase the level when it
    // hits zero. Returns false (and mutates nothing) when the level is missing
    // or qty would over-draw it -- the caller counts these anomalies.
    bool remove(uint8_t side, uint32_t price, uint32_t qty)
    {
        auto& m = side_for(side);
        auto lvl = m.find(price);
        if (lvl == m.end()) return false;     // missing level (fix #8)
        if (qty > lvl->second) return false;  // over-draw: skip, no underflow (fix #9)
        lvl->second -= qty;
        if (lvl->second == 0) m.erase(lvl);
        return true;
    }
};

// --- Oracle state -----------------------------------------------------------

class Oracle
{
public:
    explicit Oracle(std::ostream& tob) : tob_(tob) {}

    void parse_loop(std::istream& file);
    void print_report(std::ostream& out) const;

private:
    // Per-message handlers. payload_ holds the current message body.
    void handle_directory(uint16_t locate);
    void handle_add(uint16_t locate, uint64_t timestamp, uint64_t msg_index);
    void handle_execute(uint16_t locate, uint64_t timestamp, uint64_t msg_index);
    void handle_delete(uint16_t locate, uint64_t timestamp, uint64_t msg_index);
    void handle_replace(uint16_t locate, uint64_t timestamp, uint64_t msg_index);

    void check_crossed_book(uint16_t locate, uint64_t timestamp, uint64_t msg_index);
    void emit_tob(uint64_t msg_index, uint64_t timestamp);

    const uint8_t* payload_ = nullptr;   // points into the read buffer for the current message

    std::ostream& tob_;

    // Message-type histogram, indexed by the type char (payload[0]). A flat
    // 256-entry array keyed by byte avoids hashing on the hot path.
    uint64_t msg_type_counts_[256] = {};

    std::unordered_map<uint16_t, uint64_t> stock_locate_count_;
    std::unordered_map<uint16_t, std::string> locate_to_symbol_;
    std::unordered_map<uint64_t, Order> order_ref_;
    std::unordered_map<uint16_t, Book> book_ref_;

    uint64_t wrong_ref_count_ = 0;
    uint64_t prev_timestamp_ = 0;
    uint64_t timestamp_violations_ = 0;
    uint64_t over_cancel_count_ = 0;
    uint64_t zero_share_orders_ = 0;   // adds/replaces resting zero shares
    uint64_t crossed_book_count_ = 0;
    uint64_t aapl_crossed_count_ = 0;
    uint64_t crossed_examples_logged_ = 0;
    char last_s_ = 0;
};

void Oracle::check_crossed_book(uint16_t locate, uint64_t timestamp, uint64_t msg_index)
{
    auto& bk = book_ref_[locate];
    if (bk.bids.empty() || bk.asks.empty()) return;
    const auto best_bid = bk.bids.rbegin()->first;
    const auto best_ask = bk.asks.begin()->first;
    if (best_bid < best_ask) return;

    // Counts both locked (==) and crossed (>) books -- intended per spec.
    crossed_book_count_++;
    if (locate == AAPL) aapl_crossed_count_++;

    if (crossed_examples_logged_ < 15)
    {
        const uint64_t secs = timestamp / NS_PER_SEC;   // ns since midnight -> s
        const int hh = static_cast<int>(secs / 3600);
        const int mm = static_cast<int>((secs / 60) % 60);
        const int ss = static_cast<int>(secs % 60);
        std::cerr << "CROSS msg#" << msg_index
                  << "  " << locate_to_symbol_[locate]
                  << "  t=" << hh << ":" << (mm < 10 ? "0" : "") << mm
                  << ":" << (ss < 10 ? "0" : "") << ss
                  << "  bid=" << best_bid / static_cast<double>(PRICE_SCALE)
                  << "  ask=" << best_ask / static_cast<double>(PRICE_SCALE)
                  << (best_bid == best_ask ? "  (locked)" : "  (crossed)")
                  << "\n";
        crossed_examples_logged_++;
    }
}

void Oracle::emit_tob(uint64_t msg_index, uint64_t timestamp)
{
    auto& bk = book_ref_[AAPL];
    uint32_t bid_px = 0, bid_qty = 0, ask_px = 0, ask_qty = 0;   // 0,0 = empty side
    if (!bk.bids.empty()) { auto b = bk.bids.rbegin(); bid_px = b->first; bid_qty = b->second; }
    if (!bk.asks.empty()) { auto a = bk.asks.begin();  ask_px = a->first; ask_qty = a->second; }
    tob_ << msg_index << ',' << timestamp << ','
         << bid_px << ',' << bid_qty << ',' << ask_px << ',' << ask_qty << '\n';
}

void Oracle::handle_directory(uint16_t locate)
{
    locate_to_symbol_[locate] =
        std::string(reinterpret_cast<const char*>(payload_ + R_SYMBOL_OFFSET), R_SYMBOL_LEN);
}

void Oracle::handle_add(uint16_t locate, uint64_t timestamp, uint64_t msg_index)
{
    const auto order_ref_number = read_be<8>(payload_ + ADD_REF_OFFSET);
    const auto shares = static_cast<uint32_t>(read_be<4>(payload_ + ADD_SHARES_OFFSET));
    const auto raw_price = static_cast<uint32_t>(read_be<4>(payload_ + ADD_PRICE_OFFSET));
    const uint8_t side = payload_[ADD_SIDE_OFFSET] == 'B' ? 1 : 0;

    order_ref_[order_ref_number] = Order{locate, side, raw_price, shares};
    book_ref_[locate].add(side, raw_price, shares);

    if (shares == 0) zero_share_orders_++;

    check_crossed_book(locate, timestamp, msg_index);
    if (locate == AAPL) emit_tob(msg_index, timestamp);
}

void Oracle::handle_execute(uint16_t locate, uint64_t timestamp, uint64_t msg_index)
{
    const auto order_ref_number = read_be<8>(payload_ + EXEC_REF_OFFSET);
    const auto executed = static_cast<uint32_t>(read_be<4>(payload_ + EXEC_SHARES_OFFSET));

    auto it = order_ref_.find(order_ref_number);
    if (it == order_ref_.end()) { wrong_ref_count_++; return; }
    auto& [stock_locate, side, raw_price, shares] = it->second;

    // Over-execution: count once per violating message, do not underflow.
    if (executed > shares)
    {
        over_cancel_count_++;
    }
    else
    {
        shares -= executed;
        // remove() also guards the missing-level / over-draw cases; only the
        // qty mismatch is counted here, mirroring the resting-order check.
        book_ref_[stock_locate].remove(side, raw_price, executed);
        if (shares == 0) order_ref_.erase(order_ref_number);
    }

    check_crossed_book(locate, timestamp, msg_index);
    if (locate == AAPL) emit_tob(msg_index, timestamp);
}

void Oracle::handle_delete(uint16_t locate, uint64_t timestamp, uint64_t msg_index)
{
    const auto order_ref_number = read_be<8>(payload_ + DELETE_REF_OFFSET);

    auto it = order_ref_.find(order_ref_number);
    if (it == order_ref_.end()) { wrong_ref_count_++; return; }
    auto& [stock_locate, side, raw_price, shares] = it->second;

    book_ref_[stock_locate].remove(side, raw_price, shares);
    order_ref_.erase(it);

    check_crossed_book(locate, timestamp, msg_index);
    if (locate == AAPL) emit_tob(msg_index, timestamp);
}

void Oracle::handle_replace(uint16_t locate, uint64_t timestamp, uint64_t msg_index)
{
    const auto original_ref_number = read_be<8>(payload_ + U_ORIGREF_OFFSET);
    const auto new_ref_number = read_be<8>(payload_ + U_NEWREF_OFFSET);
    const auto new_shares = static_cast<uint32_t>(read_be<4>(payload_ + U_SHARES_OFFSET));
    const auto new_price = static_cast<uint32_t>(read_be<4>(payload_ + U_PRICE_OFFSET));

    auto it = order_ref_.find(original_ref_number);
    if (it == order_ref_.end()) { wrong_ref_count_++; return; }
    auto& [stock_locate, side, raw_price, shares] = it->second;

    auto& bk = book_ref_[stock_locate];
    bk.remove(side, raw_price, shares);
    bk.add(side, new_price, new_shares);

    if (new_shares == 0) zero_share_orders_++;

    order_ref_[new_ref_number] = Order{stock_locate, side, new_price, new_shares};
    order_ref_.erase(original_ref_number);

    check_crossed_book(locate, timestamp, msg_index);
    if (locate == AAPL) emit_tob(msg_index, timestamp);
}

void Oracle::parse_loop(std::istream& file)
{
    uint8_t len[2] = {};
    uint8_t payload[MAX_PAYLOAD] = {};
    payload_ = payload;

    uint64_t msg_index = 0;
    while (true)
    {
        file.read(reinterpret_cast<char*>(len), 2);
        if (file.gcount() == 0) break;
        const uint16_t N = static_cast<uint16_t>(read_be<2>(len));
        if (N > sizeof(payload))
        {
            std::cerr << "size of payload greater than buffer.";
            return;
        }
        file.read(reinterpret_cast<char*>(payload), N);
        if (file.gcount() != N)
        {
            std::cerr << "Drift detected.\n";
            return;
        }

        const char type = static_cast<char>(payload[0]);
        msg_type_counts_[payload[0]]++;
        const uint64_t this_msg = msg_index++;   // 0-based, every framed message
        const auto locate = static_cast<uint16_t>(read_be<2>(payload + STOCK_LOCATE_OFFSET));
        const auto timestamp = read_be<6>(payload + TIMESTAMP_OFFSET);

        // Monotonic-timestamp check against the immediately previous message;
        // always advance prev so a single dip doesn't poison the rest (fix #11).
        if (timestamp < prev_timestamp_) timestamp_violations_++;
        prev_timestamp_ = timestamp;

        stock_locate_count_[locate]++;

        switch (type)
        {
        case 'S': last_s_ = static_cast<char>(payload[S_EVENTCODE_OFFSET]); break;
        case 'R': handle_directory(locate); break;
        case 'A':
        case 'F': handle_add(locate, timestamp, this_msg); break;
        case 'E':
        case 'C':
        case 'X': handle_execute(locate, timestamp, this_msg); break;
        case 'D': handle_delete(locate, timestamp, this_msg); break;
        case 'U': handle_replace(locate, timestamp, this_msg); break;
        default: break;
        }
    }
}

void Oracle::print_report(std::ostream& out) const
{
    for (int i = 0; i < 256; i++)
    {
        if (msg_type_counts_[i] != 0)
        {
            out << static_cast<char>(i) << ": " << msg_type_counts_[i] << std::endl;
        }
    }

    out << "Crossed book count (all symbols): " << crossed_book_count_ << "\n";
    out << "Crossed book count (AAPL): " << aapl_crossed_count_ << "\n";

    if (wrong_ref_count_)      out << "Wrong order referenced count: " << wrong_ref_count_ << "\n";
    if (timestamp_violations_) out << "Timestamp violations: " << timestamp_violations_ << "\n";
    if (over_cancel_count_)    out << "Over cancel count: " << over_cancel_count_ << "\n";
    if (zero_share_orders_)    out << "Adding failures: " << zero_share_orders_ << "\n";
    if (last_s_ == 'C')        out << "Last S was a C\n";

    std::vector<std::pair<uint16_t, uint64_t>> by_volume(
        stock_locate_count_.begin(), stock_locate_count_.end());
    std::ranges::sort(by_volume, [](const auto& a, const auto& b) { return a.second > b.second; });

    const std::size_t top = std::min<std::size_t>(10, by_volume.size());
    for (std::size_t i = 0; i < top; i++)
    {
        auto sym = locate_to_symbol_.find(by_volume[i].first);
        out << (sym == locate_to_symbol_.end() ? std::string{} : sym->second)
            << " " << by_volume[i].second << "\n";
    }
}

}  // namespace

int main(const int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <filename>" << std::endl;
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open file: " << argv[1] << "\n";
        return 1;
    }

    // ToB regression trace for the diff symbol (spec 7.1): one line per AAPL
    // book-modifying message, emitted after applying it. Raw integer prices.
    std::filesystem::create_directories("../results");
    std::ofstream tob("../results/oracle_AAPL.tob");
    if (!tob)
    {
        std::cerr << "Could not open ../results/oracle_AAPL.tob\n";
        return 1;
    }
    tob << "msg_index,timestamp,best_bid_px,best_bid_qty,best_ask_px,best_ask_qty\n";

    Oracle oracle(tob);
    oracle.parse_loop(file);
    oracle.print_report(std::cout);

    return 0;
}
