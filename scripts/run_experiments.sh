#!/usr/bin/env bash
# Run every experiment and write its CSVs to results/.
#
# Pinning to a single core is not optional on a laptop: without it, the scheduler migrates
# the process between cores with cold caches and different thermal headroom, and the
# load-factor sweep came out non-monotonic purely from that. It does not make the machine
# quiet -- see the noise line each experiment prints -- but it removes one large source of
# variance for free.
set -euo pipefail

BIN="${BIN:-build/release/bin}"
CPU="${CPU:-3}"
OUT="${MAPLAB_RESULTS_DIR:-results}"
export MAPLAB_RESULTS_DIR="$OUT"

if [[ ! -x "$BIN/exp_simd_vs_scalar" ]]; then
  echo "error: $BIN/exp_simd_vs_scalar not found. Build first:" >&2
  echo "  cmake --preset release && cmake --build --preset release" >&2
  exit 1
fi

mkdir -p "$OUT"

PIN=()
if command -v taskset >/dev/null 2>&1; then
  PIN=(taskset -c "$CPU")
  echo "pinning to CPU $CPU"
fi

gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)
if [[ "$gov" != "performance" ]]; then
  echo "note: cpufreq governor is '$gov'. For less noise:"
  echo "  sudo cpupower frequency-set -g performance"
fi

for exp in exp_simd_vs_scalar exp_load_factor exp_hash_quality exp_h2_filter \
           exp_tombstones exp_group_size exp_memory; do
  echo
  echo "### $exp"
  "${PIN[@]}" "$BIN/$exp" | tee "$OUT/$exp.txt"
done

echo
echo "CSVs written to $OUT/. Render the figures with:"
echo "  python3 bench/plots/plot.py --results $OUT --out docs/img"
