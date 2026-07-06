// tsc.h — rdtsc/lfence timer wrapper + cycles->ns calibration (Rung 1, spec §8)
//
// WEEK 2 — leave empty during correctness bring-up. Notes for later:
//   - rdtsc on the isolated core; invariant TSC, calibrate cycles->ns once
//     (>=100ms spin vs CLOCK_MONOTONIC).
//   - lfence to serialize; on AMD verify MSR C001_1029[1], else use rdtscp.
//   - No clock_gettime on the hot path; warm up the timer first.

#pragma once
