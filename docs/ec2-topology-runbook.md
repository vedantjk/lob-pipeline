# EC2 topology sweep — SPSC handoff across the full coherence ladder

Goal: extend WRITEUP §8.1's two-point topology table (HT-sibling, same-L3) with
the tiers the dev box physically can't produce — **cross-CCD** (same socket,
different L3) and **cross-NUMA** (different sockets). One script, one CSV.

## 1. Instance choice

The instance must give you real topology, so **prefer `.metal`** — on virtualized
instances the guest-visible topology is a presentation and the hypervisor adds
jitter you can't remove.

| Instance | Why | Tiers you get |
|---|---|---|
| **`c6a.metal`** (recommended) | dual-socket EPYC Milan, 2×48 cores, multiple CCDs per socket, SMT on | all four |
| `m6a.metal` / `r6a.metal` | same silicon, more RAM (irrelevant here) — use if capacity is tight | all four |
| `m7i.metal-48xl` | dual-socket Intel — no CCD concept, so cross-l3 tier ≈ absent, but clean cross-NUMA | three |
| `c6a.48xlarge` (virtualized fallback) | full box, so topology is *usually* honest | all four, with a jitter asterisk |

Any Linux AMI is fine (Ubuntu 22.04+ / AL2023). No GPU, no special networking —
this is a pure CPU/coherence experiment.

## 2. Setup (5 minutes)

```bash
sudo apt-get update && sudo apt-get install -y g++ cmake git   # AL2023: dnf install gcc-c++ cmake git
git clone https://github.com/vedantjk/lob-pipeline.git
cd lob-pipeline
scripts/topology_sweep.sh          # builds the two bench targets itself
```

The script:
- auto-detects one core pair per tier from `lscpu -p` (prefers high-numbered
  CPUs, which are less likely to carry housekeeping),
- runs `rung6_spsc_bench` (padded) and `_nopad` (false-sharing layout), 4 reps
  each by default,
- writes `results/topology_sweep.csv` + raw logs + an environment snapshot
  (`results/topology_raw/env.txt`, including the instance type via IMDS).

Knobs: `REPS=8 N=4000000 scripts/topology_sweep.sh`; manual pairs via
`PAIRS="cross-numa:96:0 cross-l3:24:0" scripts/topology_sweep.sh`.

Nothing needs root. Total runtime is a couple of minutes.

## 3. Optional: quieter tail (root, metal only)

The defaults give solid p50/p99. If you also want a citable p99.9, isolate the
chosen cores first (pick them from the script's `[topo]` output, then re-run
with `PAIRS=` fixed to those cores):

```bash
# /etc/default/grub GRUB_CMDLINE_LINUX, then update-grub && reboot:
isolcpus=<cores> nohz_full=<cores> rcu_nocbs=<cores>
```

Skip this on a shared/borrowed box — the p50/p99 story doesn't need it.

## 4. Reading the results

- **Expected ordering:** ht-sibling < same-l3 < cross-l3 < cross-numa (p50).
  Each step adds a longer coherence path: shared L1/L2 → shared L3 → over the
  on-package fabric between CCDs → over the inter-socket link.
- **Use medians across reps for p50/p99.** Ignore single-run p99.9 outliers
  unless you did §3.
- **`skew_dropped` must be 0.** The bench measures with rdtsc deltas across
  cores; nonzero means the two cores' TSCs are not in one synced domain (most
  plausible cross-socket) and that tier's latencies are then untrustworthy —
  the guard drops those samples rather than letting them poison the tail. If
  cross-numa shows persistent skew, report the throughput but not the latency
  for that tier, and say why.
- **pad=1 vs pad=64 per tier** is a bonus result: how the false-sharing penalty
  scales as the bounced line's round-trip gets longer.

## 5. Folding into the writeup

Add rows to the §8.1 table (medians over reps), note the instance type + core
pair per tier from `env.txt`, and keep the existing dev-box caveat structure:
one sentence on what the box was, one on what wasn't controlled. Copy
`topology_sweep.csv` and `env.txt` off the box before teardown —
`results/` is gitignored, they will not be in a commit.

## 6. Teardown

Terminate the instance when the CSV is off the box. Metal billing is per-second
after the first minute, but a forgotten `.metal` is an expensive pet.
