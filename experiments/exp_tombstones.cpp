// Experiment 5: tombstone accumulation under churn.
//
// Question:  A flat table cannot simply empty a slot on erase -- doing so would truncate
//            any probe chain running through it. It leaves a tombstone instead, which
//            costs a slot that neither holds an element nor terminates a probe. What
//            happens to a table that churns forever, and does the drain policy fix it?
// Method:    Pin the element count and run insert/erase pairs at constant size, so nothing
//            grows because the table is filling with *elements*. Sample throughput,
//            capacity and tombstone count every batch, for two configurations differing in
//            one bool. The workload is deterministic, so the timed pass (stats-free) and
//            the instrumented pass (which supplies the grow/drain event counts) see
//            exactly the same sequence of table states.
// Expect:    Without the drain, growth_left is consumed by tombstones rather than
//            elements, so the table grows even though its size never changes: memory
//            climbs, the working set leaves cache, and lookup cost climbs with it. With
//            the drain, capacity should be flat and the cost should be stationary, at the
//            price of a periodic rehash.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "exp_common.hpp"
#include "maplab/flat_map.hpp"
#include "maplab_workloads/workloads.hpp"

namespace {

using namespace maplab_exp;

struct sample {
  double ns_per_op;
  std::size_t size;
  std::size_t capacity;
  std::size_t tombstones;
  std::size_t bytes;
  std::uint64_t grows;
  std::uint64_t drains;
};

template<class Config>
using table =
    maplab::flat_map<std::uint64_t, std::uint64_t, maplab::default_hash, std::equal_to<>, Config>;

// One churning table, driven a batch at a time. Keeping the cursor state with the table
// lets the two variants be advanced in lockstep.
template<class Config>
struct churner {
  table<Config> m;
  std::uint64_t next;
  std::uint64_t oldest = 0;

  explicit churner(std::size_t working_set) : next(working_set) {
    m.reserve(working_set);
    for (std::uint64_t k = 0; k < working_set; ++k) m.try_emplace(k, k);
  }

  // Returns ns per operation for this batch.
  double batch(std::size_t ops, std::size_t working_set) {
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < ops; ++i) {
      m.try_emplace(next++, 0);
      m.erase(oldest++);
      // A lookup per churn pair, because the cost of tombstones is paid by readers, not
      // by the writer that created them.
      sink(m.find(oldest + working_set / 2) != m.end());
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(ops);
  }
};

// The two variants are advanced **in lockstep**, one batch each, alternating. Running all
// of one and then all of the other would measure the second on a hotter and therefore
// slower-clocked CPU, which is precisely the bias this comparison must not have.
template<class PlainA, class PlainB, class StatsA, class StatsB>
void run_pair(const char* label_a,
              const char* label_b,
              std::size_t working_set,
              std::size_t batches,
              std::size_t batch_ops,
              csv& out) {
  std::vector<sample> a;
  std::vector<sample> b;
  a.reserve(batches);
  b.reserve(batches);

  {
    churner<PlainA> ca(working_set);
    churner<PlainB> cb(working_set);
    for (std::size_t i = 0; i < batches; ++i) {
      const double na = ca.batch(batch_ops, working_set);
      const double nb = cb.batch(batch_ops, working_set);
      a.push_back({na, ca.m.size(), ca.m.capacity(), ca.m.tombstones(), ca.m.memory_usage(), 0, 0});
      b.push_back({nb, cb.m.size(), cb.m.capacity(), cb.m.tombstones(), cb.m.memory_usage(), 0, 0});
    }
  }

  // A second, untimed lockstep pass supplies the rehash event counts. The workload is
  // deterministic, so these are the same table states as above.
  {
    churner<StatsA> ca(working_set);
    churner<StatsB> cb(working_set);
    for (std::size_t i = 0; i < batches; ++i) {
      ca.batch(batch_ops, working_set);
      cb.batch(batch_ops, working_set);
      a[i].grows = ca.m.stats().grows;
      a[i].drains = ca.m.stats().drains;
      b[i].grows = cb.m.stats().grows;
      b[i].drains = cb.m.stats().drains;
    }
  }

  const console_table console({{"batch", 7},
                               {"ns/op", 9},
                               {"size", 9},
                               {"capacity", 10},
                               {"tombstones", 11},
                               {"KiB", 9},
                               {"grows", 7},
                               {"drains", 7}});
  for (const auto& [label, rows] : {std::pair{label_a, &a}, std::pair{label_b, &b}}) {
    std::cout << "\n-- " << label << '\n';
    for (std::size_t i = 0; i < rows->size(); ++i) {
      const auto& s = (*rows)[i];
      // Print every batch for the first few, then every fifth, so the table stays
      // readable while the trend stays visible.
      if (i < 3 || i % 20 == 0 || i + 1 == rows->size()) {
        console.row({std::to_string(i),
                     fmt(s.ns_per_op),
                     std::to_string(s.size),
                     std::to_string(s.capacity),
                     std::to_string(s.tombstones),
                     std::to_string(s.bytes / 1024),
                     std::to_string(s.grows),
                     std::to_string(s.drains)});
      }
      out.row(label,
              i,
              fmt(s.ns_per_op, 4),
              s.size,
              s.capacity,
              s.tombstones,
              s.bytes,
              s.grows,
              s.drains);
    }
  }
}

// A batch cannot be repeated -- it advances the table's state -- so the per-batch timing
// is single-shot and noisy. Comparing the *minimum* within each third of the run is the
// same estimator every other experiment uses (least-throttled sample wins) applied to a
// series that cannot be re-run, and it keeps the conclusion out of the hands of whichever
// batch happened to land last.
void summarise(const std::string& csv_path) {
  std::ifstream in(csv_path);
  std::string line;
  std::getline(in, line);  // header
  std::map<std::string, std::vector<double>> series;
  std::map<std::string, std::pair<std::size_t, std::size_t>> final_state;
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    std::string cell;
    std::istringstream ss(line);
    while (std::getline(ss, cell, ',')) f.push_back(cell);
    if (f.size() < 7) continue;
    series[f[0]].push_back(std::stod(f[2]));
    final_state[f[0]] = {std::stoul(f[4]), std::stoul(f[6])};  // capacity, bytes
  }

  std::cout << "\nSummary -- minimum ns/op within each third of the run (the least\n"
               "throttled sample, as everywhere else in this suite):\n\n";
  const console_table console({{"variant", 16},
                               {"first third", 12},
                               {"middle third", 13},
                               {"last third", 11},
                               {"final capacity", 15},
                               {"final KiB", 10}});
  for (const auto& [label, ns] : series) {
    const std::size_t third = ns.size() / 3;
    const auto seg_min = [&](std::size_t lo, std::size_t hi) {
      return *std::min_element(ns.begin() + static_cast<long>(lo),
                               ns.begin() + static_cast<long>(hi));
    };
    console.row({label,
                 fmt(seg_min(0, third)),
                 fmt(seg_min(third, 2 * third)),
                 fmt(seg_min(2 * third, ns.size())),
                 std::to_string(final_state[label].first),
                 std::to_string(final_state[label].second / 1024)});
  }
}

}  // namespace

int main() {
  heading("Experiment 5: tombstones under sustained churn");
  std::cout << "Constant element count throughout. Nothing here grows because the table\n"
               "is filling up with elements -- only because it is filling up with\n"
               "tombstones.\n";

  csv out(results_dir() + "/exp5_tombstones.csv",
          {"variant",
           "batch",
           "ns_per_op",
           "size",
           "capacity",
           "tombstones",
           "bytes",
           "grows",
           "drains"});

  constexpr std::size_t working_set = 1U << 16U;
  constexpr std::size_t batches = 300;
  constexpr std::size_t batch_ops = 1U << 16U;

  run_pair<maplab::no_drain_config,
           maplab::default_config,
           maplab::no_drain_stats_config,
           maplab::stats_config>(
      "drain disabled", "drain enabled", working_set, batches, batch_ops, out);

  summarise(out.path());

  std::cout << "\nWatch the capacity column rather than the size column. A table whose element\n"
               "count never changes has no business growing, and the version that does is paying\n"
               "for erases it performed hundreds of thousands of operations earlier.\n"
               "\n"
               "Unlike every other experiment here, a batch cannot be repeated and a minimum\n"
               "taken: each batch advances the table's state, so the timing column is single-shot\n"
               "and correspondingly noisy. It is the capacity and footprint columns that carry\n"
               "this result; the time follows them once the larger table stops fitting in cache.\n";
  return 0;
}
