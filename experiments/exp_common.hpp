// Shared harness for the ablation experiments.
//
// The experiments do not use Google Benchmark. That is deliberate: an experiment reports
// a *pair* of numbers -- a time and the probe-level counters that explain it -- for a
// grid of compile-time configurations, and it writes a tidy CSV that bench/plots/plot.py
// turns into a figure. Google Benchmark is the right tool for "how fast is this", and the
// wrong one for "why".
//
// Timing method, stated once and applied everywhere:
//   * a warm-up pass, then `repetitions` timed passes, and we report the **minimum**.
//     Timing noise on a laptop is one-sided -- an interrupt, a frequency drop, another
//     process -- so the fastest observed pass is the best estimate of the cost of the code
//     itself, and the median drifts with whatever else the machine was doing. The first
//     run of Experiment 2 reported a *non-monotonic* load-factor curve for exactly this
//     reason while its counters were perfectly monotonic.
//   * every measurement is repeated internally until it spans at least 20 ms. A pass over
//     1024 lookups takes about 5 microseconds, and a single scheduler interrupt can
//     inflate that fifty-fold; no choice of estimator repairs a measurement that short.
//   * the spread (max-min)/min is tracked across every measurement in a run and printed at
//     the end, so a reader can see how much to trust the timing columns. When the spread
//     is large, the counter columns are the signal and the nanoseconds are the anecdote.
//   * the measured region always performs a fixed number of operations, so the reported
//     number is ns/op and is comparable across rows.
//   * **timing and counting are separate runs.** A probe_stats update is several stores
//     and a histogram increment per lookup, against a 400-byte counter block that
//     competes for the same L1 the table wants. Timing an instrumented build measures the
//     instrument: it inflated this project's first Experiment 1 run by roughly 8x at
//     64K elements. Every experiment therefore times a stats-free table and collects
//     counters from its stats-enabled twin -- the same configuration in every other
//     respect, so the two runs place the same keys in the same slots.

#ifndef MAPLAB_EXP_COMMON_HPP
#define MAPLAB_EXP_COMMON_HPP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "maplab/flat_map.hpp"

namespace maplab_exp {

// Keep a value alive across the optimiser without pulling in <benchmark/benchmark.h>.
template<class T>
inline void sink(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

// Hit lookups in the experiments are guaranteed hits by construction. Asserting that,
// rather than branching on it, keeps the measured loop free of a compare the real
// workload would not have -- see bench/bench_common.hpp for the same argument.
template<class It, class Map>
inline decltype(auto) value_of(It it, const Map& m) {
  MAPLAB_ASSUME(it != m.end());
  return it->second;
}

inline constexpr std::size_t default_repetitions = 7;

struct timing {
  double best;    // minimum ns/op over the repetitions
  double median;  // median ns/op, for comparison
  double spread;  // (max - min) / min, i.e. how noisy this measurement was
};

// Worst spread seen so far in this process. Reported once, at the end, by
// report_timing_noise().
inline double g_worst_spread = 0.0;

// A measurement shorter than this is repeated until it is at least this long. A pass over
// 1024 lookups takes ~5 microseconds, which one scheduler interrupt can inflate 50-fold;
// no estimator fixes that, only measuring for long enough does.
inline constexpr double min_measure_ns = 20e6;  // 20 ms

// `body` must perform exactly `ops` operations. It is called once untimed first, both to
// warm caches, branch predictors and the allocator, and to size the inner repeat count.
template<class Body>
timing time_ns_per_op(std::size_t ops, Body&& body, std::size_t repetitions = default_repetitions) {
  const auto w0 = std::chrono::steady_clock::now();
  body();
  const auto w1 = std::chrono::steady_clock::now();
  const double one_pass = std::chrono::duration<double, std::nano>(w1 - w0).count();

  std::size_t inner = 1;
  if (one_pass > 0.0 && one_pass < min_measure_ns) {
    inner = static_cast<std::size_t>(min_measure_ns / one_pass) + 1;
  }

  std::vector<double> samples;
  samples.reserve(repetitions);
  for (std::size_t r = 0; r < repetitions; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t k = 0; k < inner; ++k) body();
    const auto t1 = std::chrono::steady_clock::now();
    samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                      static_cast<double>(ops * inner));
  }
  std::sort(samples.begin(), samples.end());
  const timing t{samples.front(),
                 samples[samples.size() / 2],
                 (samples.back() - samples.front()) / samples.front()};
  if (t.spread > g_worst_spread) g_worst_spread = t.spread;
  return t;
}

// Measure several alternatives with their repetitions **interleaved**: A, B, C, A, B, C,
// ... rather than all of A, then all of B.
//
// This is not a refinement, it is a correctness fix for an ablation. Running every
// repetition of the SIMD probe and then every repetition of the scalar probe means the
// second one is measured on a hotter, and therefore slower-clocked, CPU -- on this
// reference machine, sustained load drops the clock by a factor of several. That
// systematically favours whichever alternative is measured first, which is exactly the
// bias an ablation exists to avoid. Interleaving spreads any drift across all of them.
//
// Each body must perform exactly `ops` operations. Returns one timing per body, in order.
inline std::vector<timing> time_interleaved(std::size_t ops,
                                            const std::vector<std::function<void()>>& bodies,
                                            std::size_t repetitions = default_repetitions) {
  const std::size_t n = bodies.size();
  std::vector<std::size_t> inner(n, 1);
  for (std::size_t i = 0; i < n; ++i) {
    const auto w0 = std::chrono::steady_clock::now();
    bodies[i]();
    const auto w1 = std::chrono::steady_clock::now();
    const double one_pass = std::chrono::duration<double, std::nano>(w1 - w0).count();
    if (one_pass > 0.0 && one_pass < min_measure_ns) {
      inner[i] = static_cast<std::size_t>(min_measure_ns / one_pass) + 1;
    }
  }

  std::vector<std::vector<double>> samples(n);
  for (std::size_t r = 0; r < repetitions; ++r) {
    for (std::size_t i = 0; i < n; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      for (std::size_t k = 0; k < inner[i]; ++k) bodies[i]();
      const auto t1 = std::chrono::steady_clock::now();
      samples[i].push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() /
                           static_cast<double>(ops * inner[i]));
    }
  }

  std::vector<timing> out;
  out.reserve(n);
  for (auto& v : samples) {
    std::sort(v.begin(), v.end());
    const timing t{v.front(), v[v.size() / 2], (v.back() - v.front()) / v.front()};
    if (t.spread > g_worst_spread) g_worst_spread = t.spread;
    out.push_back(t);
  }
  return out;
}

template<class Body>
double best_ns_per_op(std::size_t ops, Body&& body, std::size_t repetitions = default_repetitions) {
  return time_ns_per_op(ops, std::forward<Body>(body), repetitions).best;
}

inline void report_timing_noise() {
  std::printf(
      "\nTiming noise on this run: worst (max-min)/min across all measurements was %.1f%%.\n",
      g_worst_spread * 100.0);
  if (g_worst_spread > 0.15) {
    std::printf(
        "That is high. Timings below are best-of-%zu and still carry that much run-to-run\n"
        "variation, so read the counter columns as the result and the nanoseconds as\n"
        "corroboration. See RESULTS.md for this machine's configuration.\n",
        default_repetitions);
  }
}

// A tidy-format CSV: one row per (configuration, measurement) pair, so plotting is a
// group-by rather than a reshape.
class csv {
 public:
  csv(const std::string& path, std::vector<std::string> header)
      : path_(path), header_(std::move(header)) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    out_.open(path, std::ios::trunc);
    if (!out_) throw std::runtime_error("cannot open " + path);
    for (std::size_t i = 0; i < header_.size(); ++i) {
      out_ << header_[i] << (i + 1 == header_.size() ? '\n' : ',');
    }
  }

  template<class... Args>
  void row(Args&&... args) {
    static_assert(sizeof...(Args) > 0);
    std::size_t i = 0;
    ((out_ << (i++ ? "," : "") << args), ...);
    out_ << '\n';
    if (i != header_.size()) {
      throw std::runtime_error("csv row width " + std::to_string(i) + " != header width " +
                               std::to_string(header_.size()) + " in " + path_);
    }
  }

  const std::string& path() const { return path_; }

  ~csv() {
    out_.flush();
    std::cout << "  -> wrote " << path_ << '\n';
  }

 private:
  std::string path_;
  std::vector<std::string> header_;
  std::ofstream out_;
};

inline void heading(const std::string& title) {
  std::cout << "\n=== " << title << " "
            << std::string(title.size() < 60 ? 60 - title.size() : 0, '=') << "\n";
}

// A fixed-width console table, so an experiment is readable without opening the CSV.
class console_table {
 public:
  explicit console_table(std::vector<std::pair<std::string, int>> columns)
      : columns_(std::move(columns)) {
    for (const auto& [name, w] : columns_) std::printf("%*s  ", w, name.c_str());
    std::printf("\n");
    for (const auto& [name, w] : columns_) {
      (void)name;
      std::printf("%s  ", std::string(static_cast<std::size_t>(w < 0 ? -w : w), '-').c_str());
    }
    std::printf("\n");
  }

  void row(const std::vector<std::string>& cells) const {
    for (std::size_t i = 0; i < cells.size(); ++i) {
      std::printf("%*s  ", columns_[i].second, cells[i].c_str());
    }
    std::printf("\n");
  }

 private:
  std::vector<std::pair<std::string, int>> columns_;
};

inline std::string fmt(double v, int precision = 2) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
  return buf;
}

// Where experiment output goes. Overridable so a run can be kept aside for comparison.
inline std::string results_dir() {
  const char* env = std::getenv("MAPLAB_RESULTS_DIR");
  return env != nullptr ? std::string(env) : std::string("results");
}

}  // namespace maplab_exp

#endif  // MAPLAB_EXP_COMMON_HPP
