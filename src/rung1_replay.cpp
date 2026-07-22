#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <sched.h>
#include <sys/mman.h>

#include "pipeline.h"

int main(int argc, char** argv)
{
    const char* input = nullptr;
    [[maybe_unused]] bool emit_tob = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--emit") emit_tob = true;
        else input = argv[i];
    }
    if (!input)
    {
        std::cerr << "Usage: " << argv[0] << " <input_file> [--emit]\n";
        return 1;
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        std::cerr << "warning: mlockall failed — run may take page-fault hits\n";
    if (!pin_to_core(6)) std::cerr << "warning: could not pin to core 6\n";

    std::ifstream file(input, std::ios::binary);
    if (!file) { std::cerr << "Failed to open " << input << "\n"; return 1; }

    Book book;
    MicroPriceConsumer consumer;
    uint8_t len[2];
    uint8_t payload[MAX_PAYLOAD];
    uint64_t msg_index = 0;

#ifdef MEASURE
    const double ns_per_cycle = calibrate_ns_per_cycle();
    const uint64_t overhead = timer_overhead_cycles();
    const uint32_t core_start = current_core();

    struct Sample { uint64_t idx; uint32_t parse_cyc, book_cyc, signal_cyc; };
    static constexpr size_t CAP = 1u << 21;
    Sample* ring = new Sample[CAP];
    std::memset(ring, 0, sizeof(Sample) * CAP);
    size_t k = 0;
#else
    std::ofstream tob;
    if (emit_tob)
    {
        tob.open("../results/rung1_AAPL.tob");
        if (!tob) { std::cerr << "Could not open ../results/rung1_AAPL.tob\n"; return 1; }
        tob << "msg_index,timestamp,best_bid_px,best_bid_qty,best_ask_px,best_ask_qty\n";
    }
#endif

    while (file.read(reinterpret_cast<char*>(len), 2))
    {
        const uint16_t n = static_cast<uint16_t>(read_be<2>(len));
        // Bound the read to the buffer: n is a 16-bit prefix straight from the
        // frame; a corrupt/desynced length > MAX_PAYLOAD would smash the stack.
        // (The oracle guards this identically.) A valid ITCH body is always < 64B.
        if (n > MAX_PAYLOAD) { std::cerr << "oversize block len=" << n << ", stopping\n"; break; }
        if (!file.read(reinterpret_cast<char*>(payload), n)) break;
        if (!block_decodable(payload, n)) continue;   // too short for its type

#ifdef MEASURE
        const uint64_t t0 = now_cycles();
#endif
        const DecodedMsg m = parser(payload);
#ifdef MEASURE
        const uint64_t t1 = now_cycles();
#endif
        apply(book, m);
#ifdef MEASURE
        const uint64_t t2 = now_cycles();
#endif
        const bool is_aapl = (m.locate == AAPL);
        if (is_aapl) consumer.on_update(book.top_of_book());
#ifdef MEASURE
        const uint64_t t3 = now_cycles();
        if (is_aapl && k < CAP)
        {
            ring[k].idx = msg_index;
            ring[k].parse_cyc = static_cast<uint32_t>(t1 - t0);
            ring[k].book_cyc = static_cast<uint32_t>(t2 - t1);
            ring[k].signal_cyc = static_cast<uint32_t>(t3 - t2);
            ++k;
        }
#else
        if (emit_tob && is_aapl)
        {
            const Book::ToB t = book.top_of_book();
            tob << msg_index << ',' << m.timestamp << ','
                << t.bid_px << ',' << t.bid_qty << ','
                << t.ask_px << ',' << t.ask_qty << '\n';
        }
#endif
        ++msg_index;
    }

#ifdef MEASURE
    const uint32_t core_end = current_core();

    auto to_ns = [&](uint32_t cyc) -> uint64_t {
        const double net = (cyc > overhead) ? static_cast<double>(cyc - overhead) : 0.0;
        return static_cast<uint64_t>(net * ns_per_cycle + 0.5);
    };

    std::ofstream csv("../results/rung1_latency.csv");
    csv << "msg_index,parse_ns,book_ns,signal_ns\n";
    for (size_t i = 0; i < k; ++i)
        csv << ring[i].idx << ',' << to_ns(ring[i].parse_cyc) << ','
            << to_ns(ring[i].book_cyc) << ',' << to_ns(ring[i].signal_cyc) << '\n';

    std::cerr << "ns_per_cycle=" << ns_per_cycle
              << " tsc_ghz=" << (1.0 / ns_per_cycle)
              << " overhead_cyc=" << overhead
              << " core_start=" << core_start
              << " core_end=" << core_end
              << " samples=" << k
              << " micro=" << consumer.last << "\n";
#endif

    return 0;
}
