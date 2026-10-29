#pragma once

#include "common.hpp"
#include <yaml-cpp/yaml.h>

/** TX pulse parameters, derived from the original SDR Chirp class. */
class Chirp {
public:
  explicit Chirp(const YAML::Node& config);
  double time_offset;
  double tx_duration;
  double pulse_rep_int;
  int64_t num_pulses;
  bool phase_dither;
};
