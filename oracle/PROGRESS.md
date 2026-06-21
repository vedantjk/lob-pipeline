# Rung 0 oracle — progress

_Last updated: 2026-06-20_

## Done
- **Step 1 (framing):** parse loop reads `[u16 len][payload]`, big-endian decode,
  type histogram. Clean EOF, last `S` == `C`. ✓
- **Step 2 (liquidity scan):** per-locate counts + `R` symbol map. Diff symbol
  chosen: **AAPL = locate 13** (recorded in spec §8 step 2).
- **Step 3 (adds):** `A`/`F` → `order_ref` map + per-symbol `Book`
  (`std::map<price,qty>`).
- **Step 4 (reductions):** `E`/`C`/`X`/`D`/`U` all implemented.
  - adds use `operator[]` (create level); reductions use `find` (level must exist).
  - `U` = `D`(orig) + `A`(new) with side/locate inherited from original order.
  - Full file runs clean in ~56s, no crash, no drift.
  - **AAPL book is empty (0/0) at EOF — expected & good:** end-of-day cancels drain
    the book; zero on both sides ⇒ adds/removals balance, no leaked orders.
  - **Mid-day snapshot confirms `bid < ask`** (~3¢ spread, ~$286–287, ~3600 bid /
    ~1080 ask levels). Snapshot dumps every 1M messages. Step 4 fully closed. ✓
- **Step 5 (invariants §6): all five measured & clean.**
  - #2 live-ref 0, #3 over-cancel 0, #4 timestamp-monotonic 0, #5 positive-shares 0.
  - #1 crossed book: **AAPL 0**; all-symbols 8649 — inspected & explained: all
    pre-market (8:00–9:30), illiquid microcaps (PNRL ~$1.42, MKD ~$8), counted
    per-message-while-crossed (few real episodes). Satisfies acceptance §9.2.
  - `this_msg`/`msg_index` = 0-based global counter now exists (needed for step 6).
  - Gotcha fixed: unsigned `< 0` / `<= 0` checks are no-ops — must compare *before*
    subtracting. Over-cancel counter can double-count (order + level check).

- **Step 6 (ToB trace §7.1): done.** `results/oracle_AAPL.tob` (header +
  1,512,180 rows). Raw-int prices, `0,0` empty-side sentinel, `msg_index` = global
  0-based counter, bid via `rbegin()`. Written to `../results/` (run from `oracle/`).
  This is the Rung 1 regression target (`diff` must be empty).

- **Step 7 (acceptance §9): PASSED — Rung 0 complete.** ✓
  - §9.5 external cross-check: `.tob` now has a `timestamp` column. Sampled AAPL ToB
    vs known as-traded prices (pre 2020 4:1 split):
    - 10:00:00 → $286.83 / $286.86 (within day range $285–293)
    - 15:55:00 → $291.60 / $291.64 ≈ published close ~$291.5  ✓
  - Stronger optional check available: `martinobdl/ITCH` (C++ reconstructor, same
    emi.nasdaq source) — parser-vs-parser diff, no price lookup. Not run yet.

## RUNG 0 DONE — next is Rung 1
- Regression target: `results/oracle_AAPL.tob` (header + ~1.51M rows,
  `msg_index,timestamp,best_bid_px,best_bid_qty,best_ask_px,best_ask_qty`).
- Rung 1 must reproduce the px/qty columns byte-for-byte:
  `diff <(cut -d, -f1,3-6 oracle_AAPL.tob) <(cut -d, -f1,3-6 rung1_AAPL.tob)` empty.
  (timestamp is informational; decide whether Rung 1 emits it too / whether it's in the diff.)
- Note: NASDAQ only posts fixed historical samples (newest standard = 01302020); no live data.

## Known small cleanups
- `assert lvl != end()` in reduction handlers (a missing level segfaults cryptically;
  this is how the `free(): invalid pointer` hid before).
- Build with `-fsanitize=address` while developing to localize such bugs.
- clangd shows false `std::ranges`/unused-`<algorithm>` warnings — g++ `-std=c++20`
  compiles fine; clangd just isn't set to C++20.
