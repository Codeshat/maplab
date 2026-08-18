// Registration is programmatic rather than macro-driven so benchmark names stay
// machine-readable ("workload/implementation/size"), which is what bench/plots/plot.py
// parses. Run with:
//
//   ./build/release/bin/maplab_bench --benchmark_out=results/bench.json
//        --benchmark_out_format=json --benchmark_repetitions=5
//        --benchmark_report_aggregates_only=true
#include <benchmark/benchmark.h>

#include "bench_common.hpp"

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  maplab_bench::register_ops();
  maplab_bench::register_cache_sweep();
  maplab_bench::register_strings();
  maplab_bench::register_finance();

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
