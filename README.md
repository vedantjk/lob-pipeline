# lob-pipeline

End-to-end low-latency market-data pipeline in C++:
**MoldUDP64 feed handler → ITCH 5.0 parser → price-ladder limit order book**,
with a 4-way I/O comparison (blocking / epoll / busy-poll / io_uring) and
isolated-core latency measurement (rdtsc, `perf stat`, per-stage p50/p99/p99.9).

> WIP — built rung-by-rung. See `docs/` for design specs and `WRITEUP.md` (later) for measurements.

## Rungs
- **Rung 0** — slow/correct `std::map` reference oracle (correctness baseline every fast version diffs against)
- Rung 1 — hand-written ITCH parser + price-ladder book + measurement harness
- Rung 2 — MoldUDP64 sender + blocking `recvmmsg` receiver
- Rung 3 — epoll
- Rung 4 — busy-poll + io_uring (two configs)
- Rung 5 — stress chart + writeup
