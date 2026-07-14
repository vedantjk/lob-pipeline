#!/usr/bin/env python3
import sys
import csv


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0
    i = min(len(sorted_vals) - 1, int(p * len(sorted_vals)))
    return sorted_vals[i]


def main():
    paths = sys.argv[1:] or ["results/rung1_latency.csv"]
    cols = {}
    names = []
    for path in paths:
        with open(path) as f:
            reader = csv.reader(f)
            header = next(reader)
            if not names:
                names = header[1:]
                cols = {n: [] for n in names}
            for r in reader:
                for n, v in zip(names, r[1:]):
                    cols[n].append(int(v))

    for n in names:
        v = sorted(cols[n])
        print(f"{n:10s}  p50={pct(v, .50):7d}  p99={pct(v, .99):7d}  "
              f"p99.9={pct(v, .999):7d}  max={v[-1] if v else 0:8d}  n={len(v)}")

    if {"parse_ns", "book_ns", "signal_ns"} <= set(names):
        tts = sorted(a + b + c for a, b, c in
                     zip(cols["parse_ns"], cols["book_ns"], cols["signal_ns"]))
        print(f"{'tick2sig':10s}  p50={pct(tts, .50):7d}  p99={pct(tts, .99):7d}  "
              f"p99.9={pct(tts, .999):7d}  max={tts[-1] if tts else 0:8d}  n={len(tts)}")


if __name__ == "__main__":
    main()
