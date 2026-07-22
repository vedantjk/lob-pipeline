#pragma once
// The Rung 1 inner pipeline (book apply + signal), shared by every rung's
// frontend. Rung 2+ only change how bytes arrive; this stays fixed.

#include <cstdint>
#include <sched.h>
#include "book.h"
#include "parser.h"
#include "tsc.h"

// Callers must define _GNU_SOURCE before their first include (CPU_SET et al).
inline bool pin_to_core(int core)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

constexpr uint16_t AAPL = 13;

struct MicroPriceConsumer
{
    double last = 0.0;

    void on_update(const Book::ToB& t)
    {
        if (t.bid_qty == 0 || t.ask_qty == 0) return;
        const double denom = static_cast<double>(t.bid_qty) + static_cast<double>(t.ask_qty);
        last = static_cast<double>(t.bid_px) * (static_cast<double>(t.ask_qty) / denom)
             + static_cast<double>(t.ask_px) * (static_cast<double>(t.bid_qty) / denom);
        do_not_optimize(&last);
    }
};

// Only AAPL adds enter the book; E/C/X/D/U are dispatched for EVERY symbol but
// stay correct because ITCH order-reference numbers are globally unique per
// trading day — a non-AAPL ref can never match a resting AAPL order, so those
// lookups miss and no-op. (If this book were ever reused for a locate-partitioned
// feed with per-symbol ref namespaces, these would need an m.locate==AAPL gate.)
inline void apply(Book& book, const DecodedMsg& m)
{
    switch (m.type)
    {
    case 'A':
    case 'F': if (m.locate == AAPL) book.add_order(m.order_id, m.side, m.price, m.shares); break;
    case 'E':
    case 'C':
    case 'X': book.execute(m.order_id, m.shares); break;
    case 'D': book.delete_order(m.order_id); break;
    case 'U': book.update(m.order_id, m.new_order_id, m.price, m.shares); break;
    default: break;
    }
}
