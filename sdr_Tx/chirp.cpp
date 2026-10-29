#include "chirp.hpp"
#include <cmath>
#include <stdexcept>

/** Load only TX timing; legacy receive timing fields are not required. */
Chirp::Chirp(const YAML::Node& config) {
  const auto chirp = config["CHIRP"];
  time_offset = chirp["time_offset"].as<double>(0.1);
  tx_duration = chirp["tx_duration"].as<double>();
  pulse_rep_int = chirp["pulse_rep_int"].as<double>();
  num_pulses = chirp["num_pulses"].as<int64_t>();
  phase_dither = chirp["phase_dithering"].as<bool>(false);
  if (!std::isfinite(time_offset) || time_offset <= 0 ||
      !std::isfinite(tx_duration) || tx_duration <= 0 ||
      !std::isfinite(pulse_rep_int) || pulse_rep_int < tx_duration ||
      num_pulses < -1) {
    throw std::invalid_argument("Invalid TX timing: require positive offset/duration, PRI >= duration, and num_pulses >= -1.");
  }
}
