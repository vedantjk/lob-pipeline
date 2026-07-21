#pragma once
// Shared receive-side frontend for the I/O ladder (Rungs 2–4). A *path* answers
// "what bytes arrived and how long did acquiring them cost"; this frontend answers
// "what those bytes mean" — deframe, sequence check, parse→book→signal, and the
// measurement. Every path (blocking/epoll/busy-poll/io_uring) plugs in identically.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "mold.h"
#include "pipeline.h"

#ifdef QTIME
#include <ctime>
#include <sys/socket.h>
// Extract the kernel RX timestamp (SO_TIMESTAMPNS) from a recvmmsg control block.
// Returns CLOCK_REALTIME ns the packet entered the socket buffer, or 0 if absent.
inline uint64_t frontend_rx_ns(msghdr* mh)
{
    for (cmsghdr* c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS)
        {
            timespec ts;
            std::memcpy(&ts, CMSG_DATA(c), sizeof(ts));
            return static_cast<uint64_t>(ts.tv_sec) * NS_PER_SEC + ts.tv_nsec;
        }
    return 0;
}
#endif

struct Frontend
{
    Book book;
    MicroPriceConsumer consumer;
    uint64_t expected_seq = MOLD_FIRST_SEQ;
    uint64_t msg_index = 0;
    uint64_t gaps = 0;
    std::ofstream tob;
    bool emit_tob = false;

#ifdef MEASURE
    double ns_per_cycle = 0.0;
    uint64_t overhead = 0;
    uint32_t core_start = 0;
    struct Batch { uint32_t recv_cyc, packets, msgs, aux1, aux2; };
    struct Msg { uint64_t idx; uint32_t recv_cyc, batch_msgs, parse_cyc, book_cyc, signal_cyc; };
    static constexpr size_t BCAP = 1u << 23, MCAP = 1u << 21;
    Batch* bring = nullptr;
    Msg* mring = nullptr;
    size_t bk = 0, mk = 0;
#endif

#ifdef QTIME
    // Lightweight queueing probe: end-to-end arrival→process ns per AAPL message.
    // No per-stage rdtscp (that caps throughput); just RX-timestamp vs process time.
    static constexpr size_t QCAP = 1u << 23;
    uint64_t* qring = nullptr;
    size_t qk = 0;
#endif

    // Construct AFTER pinning the reader core — the constructor calibrates the timer.
    Frontend()
    {
#ifdef MEASURE
        ns_per_cycle = calibrate_ns_per_cycle();
        overhead = timer_overhead_cycles();
        core_start = current_core();
        bring = new Batch[BCAP]; std::memset(bring, 0, sizeof(Batch) * BCAP);
        mring = new Msg[MCAP];   std::memset(mring, 0, sizeof(Msg) * MCAP);
#endif
#ifdef QTIME
        qring = new uint64_t[QCAP]; std::memset(qring, 0, sizeof(uint64_t) * QCAP);
#endif
    }

    void open_tob(const char* path)
    {
        tob.open(path);
        if (!tob) { std::cerr << "Could not open " << path << "\n"; std::exit(1); }
        tob << "msg_index,timestamp,best_bid_px,best_bid_qty,best_ask_px,best_ask_qty\n";
        emit_tob = true;
    }

    // One acquired batch as parallel arrays. Returns false on end-of-session.
    // rx_ns (QTIME only) = per-packet kernel RX timestamp for queueing latency.
    bool process_batch(const uint8_t* const* bufs, const unsigned* lens, int n,
                       uint32_t recv_cyc, uint32_t aux1, uint32_t aux2,
                       const uint64_t* rx_ns = nullptr)
    {
#ifdef MEASURE
        uint32_t batch_msgs = 0;
        for (int i = 0; i < n; ++i)
            if (lens[i] >= MOLD_HEADER_SIZE)
            {
                const uint16_t c = static_cast<uint16_t>(read_be<2>(bufs[i] + 18));
                if (c != 0xFFFF) batch_msgs += c;
            }
        if (bk < BCAP) bring[bk++] = { recv_cyc, static_cast<uint32_t>(n), batch_msgs, aux1, aux2 };
#else
        (void)recv_cyc; (void)aux1; (void)aux2;
#endif
#ifdef QTIME
        // One process-time read per batch (all packets processed within ~µs);
        // queueing latency dwarfs that at the loads this chart cares about.
        uint64_t proc_ns = 0;
        if (rx_ns)
        {
            timespec t; clock_gettime(CLOCK_REALTIME, &t);
            proc_ns = static_cast<uint64_t>(t.tv_sec) * NS_PER_SEC + t.tv_nsec;
        }
#else
        (void)rx_ns;
#endif
        for (int i = 0; i < n; ++i)
        {
            const unsigned got = lens[i];
            const uint8_t* buf = bufs[i];
            if (got < MOLD_HEADER_SIZE) continue;

            const MoldHeader h = mold_unpack_header(buf);
            if (h.count == 0xFFFF) return false;
            if (h.count == 0) continue;

            if (h.seq > expected_seq)
            {
                std::cerr << "GAP expected=" << expected_seq << " got=" << h.seq
                          << " lost=" << (h.seq - expected_seq) << "\n";
                ++gaps;
                book.reset();   // book is unrecoverable after a drop — restart clean
            }
            else if (h.seq < expected_seq)
            {
                std::cerr << "DUP/reorder seq=" << h.seq << " expected=" << expected_seq << "\n";
                continue;
            }
            expected_seq = h.seq + h.count;
            msg_index = h.seq - MOLD_FIRST_SEQ;

            const uint8_t* cursor = buf + MOLD_HEADER_SIZE;
            const uint8_t* end = buf + got;
            uint16_t len = 0;
            while (const uint8_t* payload = mold_next_block(&cursor, end, &len))
            {
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
#ifdef QTIME
                if (rx_ns && rx_ns[i] && is_aapl && qk < QCAP && proc_ns >= rx_ns[i])
                    qring[qk++] = proc_ns - rx_ns[i];   // arrival→process queueing latency
#endif
#ifdef MEASURE
                const uint64_t t3 = now_cycles();
                if (is_aapl && mk < MCAP)
                    mring[mk++] = { msg_index, recv_cyc, batch_msgs,
                                    static_cast<uint32_t>(t1 - t0),
                                    static_cast<uint32_t>(t2 - t1),
                                    static_cast<uint32_t>(t3 - t2) };
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
        }
        return true;
    }

    // Emit CSV + percentiles. aux*_name != nullptr reports that aux column;
    // aux*_cyc converts it cycles→ns (else it's a raw count, e.g. spin iters).
    void finalize(const char* csv_path, const char* label,
                  const char* aux1_name = nullptr, bool aux1_cyc = false,
                  const char* aux2_name = nullptr, bool aux2_cyc = false)
    {
#ifdef MEASURE
        const uint32_t core_end = current_core();
        auto to_ns = [&](uint32_t cyc) -> uint64_t {
            const double net = (cyc > overhead) ? static_cast<double>(cyc - overhead) : 0.0;
            return static_cast<uint64_t>(net * ns_per_cycle + 0.5);
        };
        auto at = [](const std::vector<uint64_t>& v, double p) -> uint64_t {
            return v.empty() ? 0 : v[static_cast<size_t>(p * (v.size() - 1) + 0.5)];
        };

        std::ofstream csv(csv_path);
        csv << "msg_index,recv_ns,parse_ns,book_ns,signal_ns\n";
        std::vector<uint64_t> recv_amort, parse, book_v, signal;
        for (size_t i = 0; i < mk; ++i)
        {
            const uint64_t rns = mring[i].batch_msgs ? to_ns(mring[i].recv_cyc) / mring[i].batch_msgs : 0;
            const uint64_t pns = to_ns(mring[i].parse_cyc), kns = to_ns(mring[i].book_cyc), sns = to_ns(mring[i].signal_cyc);
            csv << mring[i].idx << ',' << rns << ',' << pns << ',' << kns << ',' << sns << '\n';
            recv_amort.push_back(rns); parse.push_back(pns); book_v.push_back(kns); signal.push_back(sns);
        }

        std::vector<uint64_t> recv_batch, aux1v, aux2v;
        for (size_t i = 0; i < bk; ++i)
        {
            recv_batch.push_back(to_ns(bring[i].recv_cyc));
            aux1v.push_back(aux1_cyc ? to_ns(bring[i].aux1) : bring[i].aux1);
            aux2v.push_back(aux2_cyc ? to_ns(bring[i].aux2) : bring[i].aux2);
        }

        for (auto* v : { &recv_amort, &parse, &book_v, &signal, &recv_batch, &aux1v, &aux2v })
            std::sort(v->begin(), v->end());
        auto line = [&](const char* nm, const std::vector<uint64_t>& v, const char* unit) {
            std::cerr << nm << " p50=" << at(v, 0.50) << " p99=" << at(v, 0.99)
                      << " p99.9=" << at(v, 0.999) << ' ' << unit << "  (n=" << v.size() << ")\n";
        };
        std::cerr << "[" << label << "] ns_per_cycle=" << ns_per_cycle << " tsc_ghz=" << (1.0 / ns_per_cycle)
                  << " overhead_cyc=" << overhead << " core=" << core_start << "->" << core_end << "\n";
        line("recv/batch", recv_batch, "ns");
        line("recv/msg  ", recv_amort, "ns");
        line("parse     ", parse, "ns");
        line("book      ", book_v, "ns");
        line("signal    ", signal, "ns");
        if (aux1_name) line(aux1_name, aux1v, aux1_cyc ? "ns" : "cnt");
        if (aux2_name) line(aux2_name, aux2v, aux2_cyc ? "ns" : "cnt");
        std::cerr << "batches=" << bk << " aapl_msgs=" << mk << "\n";
#endif
#ifdef QTIME
        // Independent of MEASURE so a combined build reports queueing too. When
        // MEASURE also owns csv_path (per-stage), write queueing to <path>.q.csv.
        auto qat = [](const std::vector<uint64_t>& v, double p) -> uint64_t {
            return v.empty() ? 0 : v[static_cast<size_t>(p * (v.size() - 1) + 0.5)];
        };
        std::vector<uint64_t> q(qring, qring + qk);
        std::sort(q.begin(), q.end());
        std::string qpath(csv_path);
#ifdef MEASURE
        qpath += ".q.csv";
#endif
        std::ofstream qcsv(qpath);
        qcsv << "queue_ns\n";
        for (uint64_t v : q) qcsv << v << '\n';
        std::cerr << "[" << label << "] queue p50=" << qat(q, 0.50) << " p99=" << qat(q, 0.99)
                  << " p99.9=" << qat(q, 0.999) << " ns  (n=" << q.size() << ")\n";
#endif
#if !defined(MEASURE) && !defined(QTIME)
        (void)csv_path;
#endif
        (void)aux1_name; (void)aux1_cyc; (void)aux2_name; (void)aux2_cyc; (void)label;
        std::cerr << "[" << label << "] processed " << msg_index << " messages, gaps=" << gaps
                  << " micro=" << consumer.last << "\n";
    }
};
