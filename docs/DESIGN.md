# Rung 1 — Design & Latency Methodology

Design rationale and the per-stage latency study for the Rung 1 parser + book +
signal. Overview and headline numbers are in the [README](../README.md); this is
the methodology, the full disclosure, and the findings.

## Measured latency (ns, pooled over 5 runs = 7.56M AAPL samples)

| stage        | p50 | p99 | p99.9 | max    |
|--------------|-----|-----|-------|--------|
| parse        | 10  | 20  | 30    | 1,102  |
| book         | 70  | 311 | 842   | 18,556 |
| signal       | 20  | 30  | 30    | 1,934  |
| **tick2sig** | 100 | 341 | 882   | 18,586 |

`tick2sig` = parse + book + signal, the tick-to-signal path a strategy sees. All
five runs were byte-identical at every percentile — the measurement is
reproducible, not a single lucky sample.

## Budget (why each stage costs what it does)

- **parse ~10 ns** — a handful of big-endian loads and a struct fill over a
  payload already hot in L1 (just read from the file). This sits *on the timer
  floor* (below), so 10 ns is an upper bound at the measurement resolution, not a
  resolved cost — parse is "too cheap to measure precisely" with a per-message
  timer.
- **book ~70 ns** — an `ankerl::unordered_dense` id-map hit plus a
  `std::map<price>` red-black-tree find (~log₂(levels) pointer hops). On this CPU
  those hops are L2 hits, not DRAM misses (see cache data), so the tree is far
  cheaper here than it would be on a smaller cache. The p99.9 (842 ns) is
  new-level `std::map` inserts + long price-level FIFO reductions.
- **signal ~20 ns** — one micro-price computation (a divide and two multiplies)
  over top-of-book. Near the floor.

## Machine and method (full disclosure)

- **CPU:** AMD Ryzen 7 9800X3D, 8 cores, **96 MB L3** (3D V-cache), 8 MB L2/core.
  Measured at 5.223 GHz (performance governor, boost on, sustained).
- **Isolation (boot):** `isolcpus=6,7,14,15 nohz_full=6,7,14,15
  rcu_nocbs=6,7,14,15 processor.max_cstate=1 mitigations=off`.
- **Isolation (run):** `cpupower -g performance`, `taskset -c 6`,
  `mlockall(MCL_CURRENT|MCL_FUTURE)`, page cache pre-warmed (file resident in
  RAM, no NVMe I/O on the timed path).
- **Timer:** `rdtscp` + trailing `lfence`. Invariant TSC nominal 4.691 GHz
  (constant + nonstop), calibrated once vs `CLOCK_MONOTONIC` over a 200 ms spin.
- **Timer overhead:** 47 cycles (~10 ns), measured as the min of 1000
  back-to-back reads, **subtracted from every sample**. Because parse and signal
  land at this floor, treat their p50s as "≤ ~10 ns," not exact.
- **Recording:** three deltas per AAPL message into a preallocated, pre-faulted
  ring; converted to ns and percentiled offline. Zero allocation/I/O on the hot
  path. Asserts compiled out (`-DNDEBUG`) for the measured build.

### perf stat (whole pipeline, all 268M messages)

```
IPC                      1.14
L1-dcache-load-misses    0.43%   <- working set is cache-resident
branch-misses            0.62%
cpu-migrations           0
context-switches         1
page-faults              117     (all startup)
```

The 0.43% L1 miss rate is the key caveat: **the 96 MB L3 masks the `std::map`
cost.** On a commodity server cache (~32 MB L3) the same binary would show a
materially worse `book` tail, because the red-black-tree pointer-chase would miss
to DRAM instead of hitting L2. These numbers are honest *for this machine* and
should not be read as machine-independent.

## Findings

- **The `book` max was an `ankerl` resize.** Initially 161 µs. Root-caused as the
  id-map reallocating + rehashing its contiguous store on a power-of-two growth
  (deterministic across runs, magnitude far above any O(log n) op). Fixed by
  reserving the id-map to the provable peak (`id_to_order_.reserve(pool_cap)`) —
  max dropped 161 µs → 18.6 µs, all six 30–160 µs outliers eliminated.
- **The residual 18 µs max is `std::map` heap growth** during the market-open
  price-level flood (deterministic, morning window only). `std::map` has no
  `reserve()`; eliminating it requires replacing the tree with a dense price
  ladder — a later rung. This residual is the *motivation* for that rung,
  reported rather than hidden.

## Diagnostics that made the findings possible

- **Determinism test.** Re-running and confirming the same `msg_index` values
  spike at the same magnitude ruled out scheduler/interrupt noise and pointed at
  data-dependent work — which is how the `ankerl` resize was isolated.
- **Cross-stage correlation.** parse/book/signal are stamped separately, so a
  spike confined to one stage (vs. all three) distinguishes internal work from an
  external whole-loop stall.
- **Interrupt accounting.** `isolcpus` is scheduler-only; interrupts still land on
  the isolated core (`/proc/interrupts` shows NVMe/device IRQs on cpu6). Warming
  the page cache removes NVMe I/O from the timed path; the residual firmware SMIs
  are the irreducible floor no userspace can control.

## Scope

In: single-machine per-stage latency (parse/book/signal), reproducible,
attributed. Deferred to later rungs: MoldUDP64 ingress, SPSC queue + consumer
thread, the 4-way I/O comparison, and the p99.9-vs-load stress chart.
