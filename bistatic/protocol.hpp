/** @file One-way RF packet format and differential BPSK modem, independent of UHD. */
#pragma once
#include <algorithm>
#include <array>
#include <complex>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace bistatic {
constexpr double kPi = 3.14159265358979323846;
constexpr std::array<double, 3> kSyncPhases{{0.20, 0.30, 0.40}};
constexpr uint64_t kLeadEdges = 3;
constexpr size_t kPacketBytes = 44;

/** A fresh attempt identifies one future physical PPS edge, not a host timestamp. */
struct Packet {
  uint64_t session = 0;
  uint64_t cycle = 0;
  uint64_t attempt = 0;
  uint64_t config = 0;
  uint8_t repetition = 0;
};

/** CRC detects corrupted control messages; this protocol is not authenticated. */
inline uint32_t crc32(const uint8_t* bytes, size_t count) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < count; ++i) {
    crc ^= bytes[i];
    for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

inline void appendInt(std::vector<uint8_t>& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

inline uint64_t readInt(const std::vector<uint8_t>& bytes, size_t offset, size_t count) {
  uint64_t result = 0;
  for (size_t i = 0; i < count; ++i) result |= uint64_t(bytes.at(offset + i)) << (8 * i);
  return result;
}

/** Encode a fixed-size little-endian control packet. */
inline std::vector<uint8_t> encodePacket(const Packet& packet) {
  if (packet.repetition >= kSyncPhases.size()) throw std::invalid_argument("Invalid sync repetition");
  std::vector<uint8_t> bytes{'O', 'R', 'C', 'B', 2, 1, packet.repetition, 0};
  for (auto value : {packet.session, packet.cycle, packet.attempt, packet.config}) appendInt(bytes, value, 8);
  appendInt(bytes, crc32(bytes.data(), bytes.size()), 4);
  return bytes;
}

/** Version 2 accepts only one-way sync announcements, rejecting the old handshake. */
inline Packet decodePacket(const std::vector<uint8_t>& bytes) {
  if (bytes.size() != kPacketBytes || bytes[0] != 'O' || bytes[1] != 'R' ||
      bytes[2] != 'C' || bytes[3] != 'B' || bytes[4] != 2 || bytes[5] != 1 ||
      bytes[6] >= kSyncPhases.size() || bytes[7] ||
      readInt(bytes, 40, 4) != crc32(bytes.data(), 40)) {
    throw std::invalid_argument("Invalid RF control packet");
  }
  Packet result{readInt(bytes, 8, 8), readInt(bytes, 16, 8),
                readInt(bytes, 24, 8), readInt(bytes, 32, 8), bytes[6]};
  if (!result.session || !result.cycle || !result.attempt) throw std::invalid_argument("Zero RF transaction identifier");
  return result;
}

inline bool sameAttempt(const Packet& a, const Packet& b) {
  return a.session == b.session && a.cycle == b.cycle && a.attempt == b.attempt && a.config == b.config;
}

/** Every copy maps to the same preceding PPS edge and the same future radar cycle. */
inline bool validAnnouncement(const Packet& packet, uint64_t fingerprint, double arrival_phase,
                              double tolerance, const std::optional<Packet>& previous = std::nullopt) {
  if (!packet.session || !packet.cycle || !packet.attempt || packet.config != fingerprint ||
      packet.repetition >= kSyncPhases.size() || !std::isfinite(arrival_phase) ||
      std::abs(arrival_phase - kSyncPhases[packet.repetition]) > tolerance) return false;
  return !previous || packet.session != previous->session ||
         (packet.cycle > previous->cycle && packet.attempt > previous->attempt);
}

/** Convert a configured physical pulse interval to this radio's sample clock. */
inline uint64_t pulseOffset(uint64_t pulse, double pri, double rate, double clock_ratio) {
  return static_cast<uint64_t>(std::llround(static_cast<double>(pulse) * pri * rate * clock_ratio));
}

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

/** Decoder work counters; receive-buffer overlap can revisit a candidate. */
struct DetectionStats {
  double best_score = 0;
  uint64_t preamble_hits = 0;
  uint64_t invalid_headers = 0;
  uint64_t decoded_packets = 0;
};

/** Search with differential PN correlation; CRC resolves false correlations.
 * The unambiguous carrier-offset range is +/-31.25 kHz at the fixed control rate.
 */
inline std::optional<Detection> detect(const std::vector<Sample>& samples, double threshold = 0.85,
                                       DetectionStats* stats = nullptr) {
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
    if (stats) stats->best_score = std::max(stats->best_score, score);
    if (score < threshold) continue;
    if (stats) ++stats->preamble_hits;
    auto correction = std::polar(1.f, -std::arg(correlation));
    std::vector<uint8_t> bytes(kPacketBytes, 0);
    Sample previous = sums[start + (kPreambleSymbols - 1) * kSamplesPerSymbol];
    for (size_t bit = 0; bit < kPacketBytes * 8; ++bit) {
      Sample current = sums[start + (kPreambleSymbols + bit) * kSamplesPerSymbol];
      if (std::real(current * std::conj(previous) * correction) < 0) bytes[bit / 8] |= uint8_t(1u << (bit % 8));
      previous = current;
    }
    try {
      auto packet = decodePacket(bytes);
      if (stats) ++stats->decoded_packets;
      return Detection{packet, start,
                       std::arg(correlation) * kControlRate / (2 * kPi * kSamplesPerSymbol), score};
    } catch (const std::invalid_argument&) {
      if (stats) ++stats->invalid_headers;
      // Continue searching after a false preamble or corrupt header.
    }
  }
  return std::nullopt;
}
}  // namespace bistatic
