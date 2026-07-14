# lob-pipeline — Rung 1: Fast ITCH 5.0 Parser + Cache-Dense Book

End-to-end low-latency market-data pipeline in C++, built rung-by-rung:
**MoldUDP64 feed handler → ITCH 5.0 parser → limit order book**, with a 4-way
I/O comparison (blocking / epoll / busy-poll / io_uring) and isolated-core
latency measurement.

**Rung 1** (this document) is the inner two stages — **parse** and **book
update** — plus a micro-price **signal** consumer, measured per-message with
`rdtscp` and reported as p50/p99/p99.9. Correctness is proven by a byte-for-byte
top-of-book match against an independent reference (Rung 0).

Design rationale and full methodology: [`docs/DESIGN.md`](docs/DESIGN.md).

## Architecture

```
  ITCH 5.0 file
      │   framed:  [u16 BE length][payload]
      ▼
  ┌──────────┐   DecodedMsg    ┌───────────────────────────────┐
  │  Parser  │ ──────────────► │  Book                         │
  └──────────┘                 │   order pool + free-list      │
                               │   intrusive FIFO price levels │
                               │   std::map price index ×2     │
                               │   ankerl id-map               │
                               └──────────────┬────────────────┘
                                              │ top-of-book
                                              ▼
                                        ┌───────────┐
                                        │ Consumer  │ ──► micro-price signal
                                        └───────────┘
```

| Stage       | Responsibility                                             |
|-------------|------------------------------------------------------------|
| Framing     | `[u16 BE length][payload]` record split                    |
| Decoding    | `parser()` → `DecodedMsg` (symbol-agnostic, branch on type)|
| Book        | O(1) add/delete/execute, O(log n) price lookup             |
| Consumption | in-loop callback computing a micro-price from top-of-book  |

Network ingress (NIC → MoldUDP64 → SPSC queue → consumer thread) arrives in later
rungs; Rung 1 is a file replay so parse/book can be measured in isolation.

## Protocol Support

ITCH 5.0 over a length-prefixed file. Message types:

| Char | Message                | Book action                    |
|------|------------------------|--------------------------------|
| A/F  | Add Order (+ MPID)     | insert order, append to level  |
| E/C  | Order Executed (+price)| reduce shares / remove if empty|
| X    | Order Cancel           | reduce shares                  |
| D    | Order Delete           | remove order                   |
| U    | Order Replace          | delete old ref, add new ref    |
| S/R  | System / Directory     | parsed, not booked             |

Parse-all, book-one: every message is decoded (global `msg_index`); only
`stock_locate == 13` (AAPL) mutates the book.

## Key Data Structures

- **Order pool** — preallocated `vector<Order>` + `uint32_t` free-list, sized to
  the provable peak of live AAPL orders (27,110 → `1<<15`). Orders reference each
  other by **index, not pointer** (half the size, realloc-free, cache-dense).
- **Intrusive FIFO levels** — each price level is a doubly-linked list threaded
  through the order pool; O(1) append and unlink.
- **Price index** — `std::map<uint32_t, uint32_t> ×2` (bid/ask) mapping price to
  level index. Top-of-book is `rbegin()`/`begin()`.
- **Id-map** — `ankerl::unordered_dense::map<order_ref, order_idx>`, **reserved to
  peak** so it never resizes on the hot path (see DESIGN §Findings).
- **Micro-price consumer** — imbalance-weighted mid from top-of-book.
- **Measurement ring** — preallocated, pre-faulted array of per-message `rdtscp`
  deltas; converted to ns and percentiled offline. Zero allocation/I/O on the hot
  path.

## Measured Latency

Full session (268,744,780 messages), AAPL (1,512,179 book-mutating messages),
pooled over 5 byte-identical runs (7.56M samples). **Measured, not targeted.**

| Stage        | p50 | p99 | p99.9 | max     |
|--------------|-----|-----|-------|---------|
| parse        | 10  | 20  | 30    | 1,102   |
| book         | 70  | 311 | 842   | 18,556  |
| signal       | 20  | 30  | 30    | 1,934   |
| **tick2sig** | **100** | 341 | 882   | 18,586  |

All values in nanoseconds, timer overhead (47 cyc ≈ 10 ns) subtracted. `parse`
and `signal` sit on the timer floor — read their p50s as upper bounds. Machine,
isolation, timer, and cache caveats are in [`docs/DESIGN.md`](docs/DESIGN.md).

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Two targets from one source:

| Target        | Defines            | Purpose                                  |
|---------------|--------------------|------------------------------------------|
| `rung1`       | (asserts on)       | correctness; `--emit` writes the ToB     |
| `rung1_bench` | `MEASURE NDEBUG`   | measured run; ring buffer + latency CSV  |

```
./build/rung1 --emit data/<file>        # regenerate ToB for the diff
./scripts/run_bench.sh 5                 # cache-warm + 5 pinned runs + percentiles
```

## Correctness

```
diff <(cut -d, -f1,3-6 results/oracle_AAPL.tob) \
     <(cut -d, -f1,3-6 results/rung1_AAPL.tob)     # empty = pass
```

Top-of-book matches the Rung 0 oracle message-for-message across all 1,512,180
AAPL rows. The oracle is a structurally independent implementation
(`std::map<price, qty>` aggregate), so a shared parsing bug cannot pass the diff.

## File Layout

```
src/
  parser.h          ITCH decoder → DecodedMsg (header-only)
  book.h            order pool, FIFO levels, price index, id-map (header-only)
  tsc.h  tsc.cpp    rdtscp timer, calibration, overhead
  rung1_replay.cpp  driver: frame → parse → book → signal (+ #ifdef MEASURE)
scripts/
  run_bench.sh      cache-warm + N pinned runs + pooled percentiles
  percentiles.py    offline p50/p99/p99.9
oracle/
  reconstruct.cpp   Rung 0 reference book
third_party/ankerl/ unordered_dense (vendored, MIT)
docs/DESIGN.md      methodology + findings
```

## Requirements

- Linux, x86-64 with invariant TSC (`constant_tsc` + `nonstop_tsc`)
- C++20, GCC or Clang, CMake ≥ 3.20
- For measured runs: an isolated core (see DESIGN); otherwise runs anywhere

## Roadmap

| Rung | Adds                                                                 |
|------|----------------------------------------------------------------------|
| 0    | Slow/correct `std::map` reference oracle (correctness baseline)      |
| 1    | Hand-written ITCH parser + cache-dense book + per-stage latency (this)|
| 2    | MoldUDP64 sender + blocking `recvmmsg` receiver                      |
| 3    | epoll receiver                                                       |
| 4    | busy-poll + io_uring (two configs)                                  |
| 5    | 4-way I/O comparison, p99.9-vs-load stress chart, writeup            |
