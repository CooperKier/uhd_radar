/** @file Differential BPSK control modem; no radio or host-clock assumptions. */
#pragma once
#include "protocol.hpp"
#include <algorithm>
#include <complex>
#include <optional>

namespace bistatic {
using Sample = std::complex<float>;
constexpr size_t kSamplesPerSymbol = 16;
constexpr size_t kPreambleSymbols = 63;
constexpr double kControlRate = 1e6;

/** Fixed training sequence, independent of radar phase dithering. */
inline std::vector<float> preamble() {
  std::vector<float> result;
  uint32_t state = 0x53u;
  for (size_t i = 0; i < kPreambleSymbols; ++i) {
    state = state * 1664525u + 1013904223u;
    result.push_back(state & 0x80000000u ? -1.f : 1.f);
  }
  return result;
}

inline size_t packetSamples() { return (kPreambleSymbols + kPacketBytes * 8) * kSamplesPerSymbol; }

/** Repeat rectangular symbols; differential data decoding tolerates unknown RF phase. */
inline std::vector<Sample> modulate(const Packet& packet, float amplitude = 0.35f) {
  auto symbols = preamble();
  float previous = symbols.back();
  for (auto byte : encodePacket(packet)) {
    for (int bit = 0; bit < 8; ++bit) {
      if ((byte >> bit) & 1) previous = -previous;
      symbols.push_back(previous);
    }
  }
  std::vector<Sample> result;
  result.reserve(packetSamples());
  for (float symbol : symbols) result.insert(result.end(), kSamplesPerSymbol, Sample(amplitude * symbol, 0));
  return result;
}

struct Detection {
  Packet packet;
  size_t sample;
  double carrier_hz;
  double score;
};

/** Search with differential PN correlation; CRC resolves false correlations.
 * The unambiguous carrier-offset range is +/-31.25 kHz at the fixed control rate.
 */
inline std::optional<Detection> detect(const std::vector<Sample>& samples, double threshold = 0.85) {
  if (samples.size() < packetSamples()) return std::nullopt;
  std::vector<Sample> sums(samples.size() - kSamplesPerSymbol + 1);
  Sample sum(0, 0);
  for (size_t i = 0; i < samples.size(); ++i) {
    sum += samples[i];
    if (i >= kSamplesPerSymbol) sum -= samples[i - kSamplesPerSymbol];
    if (i + 1 >= kSamplesPerSymbol) sums[i + 1 - kSamplesPerSymbol] = sum / float(kSamplesPerSymbol);
  }
  const auto known = preamble();
  for (size_t start = 0; start + packetSamples() <= samples.size(); start += 4) {
    Sample correlation(0, 0);
    double energy = 0;
    for (size_t symbol = 1; symbol < known.size(); ++symbol) {
      auto product = sums[start + symbol * kSamplesPerSymbol] *
                     std::conj(sums[start + (symbol - 1) * kSamplesPerSymbol]);
      correlation += product * (known[symbol] * known[symbol - 1]);
      energy += std::abs(product);
    }
    double score = energy > 1e-12 ? std::abs(correlation) / energy : 0;
    if (score < threshold) continue;
    auto correction = std::polar(1.f, -std::arg(correlation));
    std::vector<uint8_t> bytes(kPacketBytes, 0);
    Sample previous = sums[start + (kPreambleSymbols - 1) * kSamplesPerSymbol];
    for (size_t bit = 0; bit < kPacketBytes * 8; ++bit) {
      Sample current = sums[start + (kPreambleSymbols + bit) * kSamplesPerSymbol];
      if (std::real(current * std::conj(previous) * correction) < 0) bytes[bit / 8] |= uint8_t(1u << (bit % 8));
      previous = current;
    }
    try {
      return Detection{decodePacket(bytes), start,
                       std::arg(correlation) * kControlRate / (2 * kPi * kSamplesPerSymbol), score};
    } catch (const std::invalid_argument&) {
      // Continue searching after a false preamble or corrupt header.
    }
  }
  return std::nullopt;
}
}  // namespace bistatic
