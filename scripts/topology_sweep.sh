#!/usr/bin/env bash
# topology_sweep.sh — run the Rung 6 SPSC cross-core handoff bench across every
# cache/NUMA topology tier the host actually has, and emit one tidy CSV.
#
# Tiers (auto-detected from `lscpu -p`; missing tiers are skipped with a note):
#   ht-sibling : two hardware threads of one physical core
#   same-l3    : two physical cores sharing an L3 (same CCX/CCD on AMD)
#   cross-l3   : two cores, different L3, same NUMA node (cross-CCD on EPYC)
#   cross-numa : two cores on different NUMA nodes (cross-socket on 2S boxes)
#
# Usage:
#   scripts/topology_sweep.sh                 # auto-pick pairs, 4 reps each
#   REPS=8 N=4000000 scripts/topology_sweep.sh
#   PAIRS="same-l3:6:7 ht-sibling:6:14" scripts/topology_sweep.sh   # manual pairs
#
# Output: results/topology_sweep.csv (+ raw logs in results/topology_raw/)
# Requires: bash, lscpu, cmake, g++ (builds the two bench targets if missing).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPS="${REPS:-4}"
N="${N:-2000000}"
OUT_DIR="$REPO_ROOT/results"
RAW_DIR="$OUT_DIR/topology_raw"
CSV="$OUT_DIR/topology_sweep.csv"
mkdir -p "$RAW_DIR"

# ---------------------------------------------------------------- build ------
BENCH="$REPO_ROOT/build/rung6_spsc_bench"
BENCH_NOPAD="$REPO_ROOT/build/rung6_spsc_bench_nopad"
if [[ ! -x "$BENCH" || ! -x "$BENCH_NOPAD" ]]; then
    echo "[build] building bench targets..."
    cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" >/dev/null
    cmake --build "$REPO_ROOT/build" --target rung6_spsc_bench rung6_spsc_bench_nopad >/dev/null
fi

# ------------------------------------------------------ topology detection ---
# lscpu -p=CPU,CORE,SOCKET,NODE,CACHE lines look like: 0,0,0,0,0:0:0:0
# CACHE is l1d:l1i:l2:l3 ids; we key CCX/CCD grouping off the l3 id.
declare -a CPUS CORES SOCKETS NODES L3S
while IFS=, read -r cpu core socket node cache; do
    [[ "$cpu" == \#* ]] && continue
    l3="${cache##*:}"
    [[ -z "$l3" ]] && l3="$socket"          # no L3 info reported: fall back
    [[ -z "$node" ]] && node="$socket"
    CPUS+=("$cpu"); CORES+=("$core"); SOCKETS+=("$socket"); NODES+=("$node"); L3S+=("$l3")
done < <(lscpu -p=CPU,CORE,SOCKET,NODE,CACHE)

NCPU=${#CPUS[@]}
echo "[topo] $NCPU logical CPUs"

# Pick the first pair satisfying a predicate. Prefer higher-numbered CPUs (they
# are less likely to be running housekeeping than cpu0/cpu1).
pick_pair() { # $1 = predicate name
    local i j
    for ((i = NCPU - 1; i >= 0; i--)); do
        for ((j = i - 1; j >= 0; j--)); do
            if "$1" "$i" "$j"; then echo "${CPUS[$i]}:${CPUS[$j]}"; return 0; fi
        done
    done
    return 1
}
is_sibling()   { [[ "${CORES[$1]}" == "${CORES[$2]}" && "${SOCKETS[$1]}" == "${SOCKETS[$2]}" ]]; }
is_same_l3()   { [[ "${CORES[$1]}" != "${CORES[$2]}" || "${SOCKETS[$1]}" != "${SOCKETS[$2]}" ]] \
              && [[ "${L3S[$1]}" == "${L3S[$2]}" && "${NODES[$1]}" == "${NODES[$2]}" ]]; }
is_cross_l3()  { [[ "${L3S[$1]}" != "${L3S[$2]}" && "${NODES[$1]}" == "${NODES[$2]}" ]]; }
is_cross_numa(){ [[ "${NODES[$1]}" != "${NODES[$2]}" ]]; }

if [[ -n "${PAIRS:-}" ]]; then
    echo "[topo] using manual PAIRS: $PAIRS"
    TIERS=($PAIRS)   # entries: tier:prod:cons
else
    TIERS=()
    for t in ht-sibling:is_sibling same-l3:is_same_l3 cross-l3:is_cross_l3 cross-numa:is_cross_numa; do
        name="${t%%:*}"; pred="${t##*:}"
        if pair=$(pick_pair "$pred"); then
            TIERS+=("$name:$pair")
            echo "[topo] $name -> cpus ${pair/:/ & }"
        else
            echo "[topo] $name -> NOT PRESENT on this host (skipped)"
        fi
    done
fi
[[ ${#TIERS[@]} -eq 0 ]] && { echo "no tiers detected; aborting"; exit 1; }

# ---------------------------------------------------------------- run --------
# Environment snapshot (goes next to the CSV; cite it with the numbers).
{
    echo "date: $(date -u +%FT%TZ)"
    echo "uname: $(uname -r)"
    echo "reps: $REPS  n: $N"
    # EC2 instance type via IMDSv2, best-effort (silently absent off-EC2).
    tok=$(curl -sS -m 1 -X PUT "http://169.254.169.254/latest/api/token" \
          -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null || true)
    [[ -n "$tok" ]] && echo "instance-type: $(curl -sS -m 1 \
          -H "X-aws-ec2-metadata-token: $tok" \
          http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null || true)"
    lscpu | grep -E "Model name|Socket|NUMA node|Core|Thread"
} > "$RAW_DIR/env.txt"
cat "$RAW_DIR/env.txt"

echo "tier,prod,cons,pad,rep,p50_ns,p99_ns,p999_ns,throughput_mmsgs,skew_dropped" > "$CSV"

run_one() { # tier prod cons pad binary rep
    local tier=$1 prod=$2 cons=$3 pad=$4 bin=$5 rep=$6
    local log="$RAW_DIR/${tier}_pad${pad}_rep${rep}.log"
    "$bin" --prod "$prod" --cons "$cons" --n "$N" 2>"$log" || { echo "[run] FAILED: $tier rep$rep (see $log)"; return 0; }
    local line p50 p99 p999 tput skew
    line=$(grep -E "^handoff" "$log" || true)
    p50=$(grep -oE "p50=[0-9]+"    <<<"$line" | cut -d= -f2)
    p99=$(grep -oE "p99=[0-9]+"    <<<"$line" | cut -d= -f2)
    p999=$(grep -oE "p99\.9=[0-9]+" <<<"$line" | cut -d= -f2)
    tput=$(grep -oE "throughput=[0-9.]+" "$log" | cut -d= -f2)
    skew=$(grep -oE "WARN: [0-9]+ TSC-skew" "$log" | grep -oE "[0-9]+" || echo 0)
    echo "$tier,$prod,$cons,$pad,$rep,$p50,$p99,$p999,$tput,$skew" >> "$CSV"
    # NB: plain `[[ ]] &&` as the last statement would return 1 when skew==0 and,
    # under set -e, kill the whole script. Use an explicit if.
    if [[ "$skew" != "0" ]]; then
        echo "[run] WARNING: $tier dropped $skew TSC-skew samples — treat this tier's numbers with suspicion"
    fi
    return 0
}

for entry in "${TIERS[@]}"; do
    IFS=: read -r tier prod cons <<<"$entry"
    echo "[run] $tier (prod=$prod cons=$cons), $REPS reps padded + $REPS nopad"
    for ((r = 1; r <= REPS; r++)); do run_one "$tier" "$prod" "$cons" 64 "$BENCH" "$r"; done
    for ((r = 1; r <= REPS; r++)); do run_one "$tier" "$prod" "$cons" 1 "$BENCH_NOPAD" "$r"; done
done

echo
echo "[done] CSV: $CSV"
echo "----------------------------------------------------------------"
column -s, -t "$CSV"
