// The set of table configurations every behavioural test runs against.
//
// The point of putting the ablations in this list is that "the scalar probe is only the
// control group for a benchmark" is not an excuse for it to be less correct. Every
// experiment variant in EXPERIMENTS.md is a build the differential test has passed.
#ifndef MAPLAB_TESTS_CONFIGS_HPP
#define MAPLAB_TESTS_CONFIGS_HPP

#include <tuple>

#include "maplab/config.hpp"

namespace maplab_tests {

using all_configs = std::tuple<maplab::default_config,               // 16-wide SSE2, 7/8
                               maplab::scalar_config,                // Experiment 1 control
                               maplab::group8_config,                // Experiment 6
                               maplab::group8_scalar_config,         //
                               maplab::no_h2_config,                 // Experiment 4
                               maplab::overlapping_hash_config,      // Experiment 4b
                               maplab::no_drain_config,              // Experiment 5
                               maplab::prefetch_config,              //
                               maplab::stats_config,                 // instrumented build
                               maplab::load_factor_config<1, 2>,     // Experiment 2
                               maplab::load_factor_config<3, 4>,     //
                               maplab::load_factor_config<15, 16>>;  // an aggressive ceiling

}  // namespace maplab_tests

#endif  // MAPLAB_TESTS_CONFIGS_HPP
