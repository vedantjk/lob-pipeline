#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

DATA=data/12302019.NASDAQ_ITCH50
N=${1:-5}
OUT=/tmp/claude-1000/-home-vedant-lob-pipeline-data/7d199d27-1ffe-4ebc-897b-002b4d58d01f/scratchpad/bench_runs
mkdir -p "$OUT"
rm -f "$OUT"/run*.csv

echo "governor: $(cat /sys/devices/system/cpu/cpu6/cpufreq/scaling_governor)"
echo "warming page cache..."
cat "$DATA" > /dev/null

for i in $(seq 1 "$N"); do
    taskset -c 6 build/rung1_bench "$DATA" 2> "$OUT/stderr$i"
    cp results/rung1_latency.csv "$OUT/run$i.csv"
    echo "--- run $i ---  $(tr -s ' ' < "$OUT/stderr$i")"
    python3 scripts/percentiles.py "$OUT/run$i.csv" | sed 's/^/    /'
done

echo ""
echo "=== POOLED across $N runs ==="
python3 scripts/percentiles.py "$OUT"/run*.csv
