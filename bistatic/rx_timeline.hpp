/** @file Align a guarded RX stream to the requested radar window using hardware timestamps. */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace bistatic {
struct RxSlice { size_t offset, count; };

/** Startup delay may consume the lead-in, but may not remove any requested radar samples. */
class RxTimeline {
  double desired_time, rate, command_time;
  uint64_t samples, guard, received = 0, aligned = 0, leading = 0, discarded = 0;
  std::optional<double> first_time;

  /** Locate the nearest sample to the desired start; reject stale or too-late streams. */
  void align(double time) {
    if ((time - command_time) * rate < -2 || (time - desired_time) * rate > 0.5)
      throw std::runtime_error("Initial radar RX timestamp is outside the capture lead-in");
    leading = discarded = uint64_t(std::llround(std::max(0.0, (desired_time - time) * rate)));
    first_time = time;
  }

 public:
  RxTimeline(double desired, double sample_rate, uint64_t radar_samples)
      : desired_time(desired), rate(sample_rate), samples(radar_samples) {
    if (!std::isfinite(desired) || !std::isfinite(rate) || rate <= 0 || !samples)
      throw std::invalid_argument("RX timeline needs finite timing, a positive rate and samples");
    guard = uint64_t(std::ceil(rate * 0.001));
    command_time = desired_time - double(guard) / rate;
  }

  double commandTime() const { return command_time; }
  double expectedTime() const { return first_time.value_or(command_time) + double(received) / rate; }
  double firstStreamTime() const { return first_time.value(); }
  double firstWindowTime() const { return first_time.value() + double(discarded) / rate; }
  uint64_t guardSamples() const { return guard; }
  uint64_t requestedSamples() const { return samples + 2 * guard; }
  uint64_t receivedSamples() const { return received; }
  uint64_t discardedSamples() const { return discarded; }
  uint64_t remaining() const { return first_time ? leading + samples - aligned : requestedSamples(); }
  bool complete() const { return aligned == samples; }

  /** Check continuity before discarding lead-in or selecting any samples for the radar. */
  RxSlice accept(bool has_time, double time, size_t count) {
    if (!has_time || !std::isfinite(time)) throw std::runtime_error("Missing or invalid radar IQ timestamp");
    if (!count || complete()) throw std::logic_error("Unexpected empty or completed RX chunk");
    if (!first_time) align(time);
    else if (std::abs((time - expectedTime()) * rate) > 2)
      throw std::runtime_error("Discontinuous radar IQ timestamps");
    size_t skip = size_t(std::min<uint64_t>(count, leading));
    size_t keep = size_t(std::min<uint64_t>(count - skip, samples - aligned));
    leading -= skip;
    aligned += keep;
    received += count;
    return {skip, keep};
  }
};
}  // namespace bistatic
