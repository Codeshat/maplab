#!/usr/bin/env bash
# Run the Google Benchmark suite and write JSON for the plotting script.
set -euo pipefail

BIN="${BIN:-build/release/bin/maplab_bench}"
OUT="${MAPLAB_RESULTS_DIR:-results}"
REPS="${REPS:-5}"
FILTER="${FILTER:-}"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found. Build first:" >&2
  echo "  cmake --preset release && cmake --build --preset release" >&2
  exit 1
fi

mkdir -p "$OUT"

PIN=()
command -v taskset >/dev/null 2>&1 && PIN=(taskset -c "${CPU:-3}")

args=(--benchmark_out="$OUT/bench.json"
      --benchmark_out_format=json
      --benchmark_repetitions="$REPS"
      --benchmark_report_aggregates_only=true
      --benchmark_min_time="${MIN_TIME:-0.2s}")
[[ -n "$FILTER" ]] && args+=(--benchmark_filter="$FILTER")

"${PIN[@]}" "$BIN" "${args[@]}"
echo "wrote $OUT/bench.json"
