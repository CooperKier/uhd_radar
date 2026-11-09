/** @file Bounded pulse-train generation and window extraction, independent of UHD. */
#pragma once
#include "modem.hpp"
#include <functional>
#include <random>

namespace bistatic {
class TrainGenerator {
  const std::vector<Sample>& chirp;
  uint64_t pulses, pulse = 0;
  double pri, rate, ratio;
  bool dither;
  std::mt19937 generator{0};
  Sample phase;
 public:
  TrainGenerator(const std::vector<Sample>& waveform, uint64_t count, double interval,
                 double sample_rate, double clock_ratio, bool phase_dither)
      : chirp(waveform), pulses(count), pri(interval), rate(sample_rate), ratio(clock_ratio),
        dither(phase_dither), phase(dither ? std::polar(1.f, static_cast<float>(generator())) : Sample(1, 0)) {}

  uint64_t samples() const { return pulseOffset(pulses - 1, pri, rate, ratio) + chirp.size(); }
  uint64_t generated() const { return pulse; }

  /** Calls must supply consecutive chunks from sample zero. */
  void fill(std::vector<Sample>& buffer, uint64_t first, size_t count) {
    if (count > buffer.size()) throw std::invalid_argument("TX chunk exceeds buffer");
    std::fill(buffer.begin(), buffer.begin() + count, Sample(0, 0));
    while (pulse < pulses) {
      uint64_t start = pulseOffset(pulse, pri, rate, ratio);
      if (start >= first + count) break;
      uint64_t stop = start + chirp.size();
      if (stop <= first) throw std::runtime_error("TX pulse generation lost its sample position");
      uint64_t left = std::max(first, start), right = std::min(first + count, stop);
      for (uint64_t sample = left; sample < right; ++sample) buffer[sample - first] = chirp[sample - start] * phase;
      if (right < stop) break;
      ++pulse;
      if (dither && pulse < pulses) phase = std::polar(1.f, static_cast<float>(generator()));
    }
  }
};

class WindowExtractor {
  uint64_t pulses, pulse = 0, received = 0;
  double pri, rate, ratio;
  bool dither;
  std::mt19937 generator{0};
  Sample phase;
  std::vector<Sample> record;
 public:
  WindowExtractor(uint64_t count, size_t samples, double interval, double sample_rate,
                  double clock_ratio, bool phase_dither)
      : pulses(count), pri(interval), rate(sample_rate), ratio(clock_ratio), dither(phase_dither),
        phase(dither ? std::polar(1.f, -static_cast<float>(generator())) : Sample(1, 0)), record(samples) {}

  uint64_t samples() const { return pulseOffset(pulses - 1, pri, rate, ratio) + record.size(); }
  uint64_t extracted() const { return pulse; }

  /** Accept contiguous samples and emit only complete, chronologically ordered windows. */
  void consume(const Sample* buffer, size_t count, const std::function<void(const std::vector<Sample>&)>& save) {
    while (pulse < pulses) {
      uint64_t start = pulseOffset(pulse, pri, rate, ratio), end = start + record.size();
      if (start >= received + count) break;
      uint64_t left = std::max(received, start), right = std::min(received + count, end);
      if (left < right) std::copy(buffer + (left - received), buffer + (right - received), record.begin() + (left - start));
      if (right < end) break;
      for (auto& sample : record) sample *= phase;
      save(record);
      ++pulse;
      if (dither && pulse < pulses) phase = std::polar(1.f, -static_cast<float>(generator()));
    }
    received += count;
  }
};
}  // namespace bistatic
