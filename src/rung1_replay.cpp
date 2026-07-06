// rung1_replay.cpp — file-replay driver: parse -> book -> ToB trace + latency CSV.
// YOU write this. (Spec §7, §9.)
//
// Week 1 shape:
//   - open the ITCH file (argv[1]); walk every framed message (parse-all).
//   - only AAPL (locate 13 / id-map membership) mutates the book (book-one, §3).
//   - emit results/rung1_AAPL.tob, byte-format identical to the oracle (§7.1).
//   - acceptance: diff <(cut -d, -f1,3-6 results/oracle_AAPL.tob) \
//                      <(cut -d, -f1,3-6 results/rung1_AAPL.tob)  is empty.
// Week 2: add the rdtsc harness + results/rung1_latency.csv (§7.2, §8).

#include <iostream>
#include <ostream>
#include <fstream>

#include "book.h"
#include "parser.h"

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <input_file>" << "\n";
    }

    std::ifstream file(argv[1], std::ios::binary);
    std::ofstream tob("../results/oracle_AAPL_rung_1.tob");
    if (!tob)
    {
        std::cerr << "Could not open ../results/oracle_AAPL_rung_1.tob\n";
        return 1;
    }

    tob << "msg_index,timestamp,best_bid_px,best_bid_qty,best_ask_px,best_ask_qty\n";

    return 0;
}
