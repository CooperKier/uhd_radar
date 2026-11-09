/** @file One-way RF sync announcements and experiment identifiers, independent of UHD. */
#pragma once
#include <array>
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
}  // namespace bistatic
