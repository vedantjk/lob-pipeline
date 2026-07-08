// rung1_replay.cpp — file-replay driver: frame -> parse -> book -> ToB trace.
// Acceptance (§7.1):
//   diff <(cut -d, -f1,3-6 results/oracle_AAPL.tob) \
//        <(cut -d, -f1,3-6 results/rung1_AAPL.tob)   is empty.

#include <cstdint>
#include <fstream>
#include <iostream>

#include "book.h"
#include "parser.h"

constexpr uint16_t AAPL = 13;

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <input_file>\n";
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) { std::cerr << "Failed to open " << argv[1] << "\n"; return 1; }

    std::ofstream tob("../results/rung1_AAPL.tob");
    if (!tob) { std::cerr << "Could not open ../results/rung1_AAPL.tob\n"; return 1; }
    tob << "msg_index,timestamp,best_bid_px,best_bid_qty,best_ask_px,best_ask_qty\n";

    Book book;
    uint8_t len[2];
    uint8_t payload[MAX_PAYLOAD];
    uint64_t msg_index = 0;

    while (file.read(reinterpret_cast<char*>(len), 2))
    {
        const uint16_t n = static_cast<uint16_t>(read_be<2>(len));
        if (!file.read(reinterpret_cast<char*>(payload), n)) break;

        const DecodedMsg m = parser(payload);
        const bool is_aapl = (m.locate == AAPL);
        bool emit = false;

        switch (m.type)
        {
        case 'A':
        case 'F':
            if (is_aapl) book.add_order(m.order_id, m.side, m.price, m.shares);
            emit = is_aapl;
            break;
        case 'E':
        case 'C':
        case 'X':
            book.execute(m.order_id, m.shares);
            emit = is_aapl;
            break;
        case 'D':
            book.delete_order(m.order_id);
            emit = is_aapl;
            break;
        case 'U':
            book.update(m.order_id, m.new_order_id, m.price, m.shares);
            emit = is_aapl;
            break;
        default:
            break;
        }

        if (emit)
        {
            const Book::ToB t = book.top_of_book();
            tob << msg_index << ',' << m.timestamp << ','
                << t.bid_px << ',' << t.bid_qty << ','
                << t.ask_px << ',' << t.ask_qty << '\n';
        }
        ++msg_index;
    }

    return 0;
}
