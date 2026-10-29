/** @file Shared, versioned waveform framing for the two-radio PPS bench PoC. */
#pragma once
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace poc {
using Sample = std::complex<float>;
static_assert(sizeof(Sample) == 8 && std::numeric_limits<float>::is_iec559,
              "PoC IQ files require two IEEE float32 values per sample.");
constexpr uint32_t kHeaderBytes = 104;
constexpr uint32_t kPreambleChips = 31;
constexpr uint32_t kPreambleRepeats = 4;
constexpr double kPi = 3.14159265358979323846;

/** Convert a time duration to an exact, bounded sample count. */
inline uint32_t sampleCount(double seconds, double rate, const char* name) {
  const double value = seconds * rate;
  if (!std::isfinite(value) || value < 1 || value > UINT32_MAX ||
      std::abs(value - std::round(value)) > 1e-5) {
    throw std::invalid_argument(std::string(name) + " must contain a positive whole number of samples.");
  }
  return static_cast<uint32_t>(std::llround(value));
}

/** Load the shared bootstrap and finite capture plan without touching hardware. */
struct Plan {
  uint32_t rate, tx_samples, rx_samples, pri_samples, total_pulses;
  uint32_t pulses_per_frame, frames, run_id, phase_seed, num_presums;
  uint32_t preamble_sps, header_sps, preamble_samples, payload_offset, frame_samples;
  uint32_t lead_samples, tail_samples;
  double start_time, arm_timeout, quiet_seconds, amplitude, frequency, bandwidth, chirp_length;
  bool phase_dither;

  explicit Plan(const YAML::Node& config) {
    const auto sync = config["SYNC"];
    const auto device = config["DEVICE"];
    const auto rf = config["RF0"];
    const auto chirp = config["CHIRP"];
    if (!sync || sync["mode"].as<std::string>("") != "shared_pps")
      throw std::invalid_argument("This PoC requires SYNC.mode: shared_pps.");
    if (device["clk_ref"].as<std::string>() != "internal" ||
        device["time_ref"].as<std::string>() != "external")
      throw std::invalid_argument("PoC requires internal frequency clock and external PPS time source.");
    if (device["cpu_format"].as<std::string>("fc32") != "fc32")
      throw std::invalid_argument("PoC captures require fc32 samples.");
    const double requested_rate = rf["tx_rate"].as<double>();
    rate = sampleCount(1.0, requested_rate, "Sample rate");
    if (rf["rx_rate"].as<double>() != requested_rate ||
        config["GENERATE"]["sample_rate"].as<double>() != requested_rate)
      throw std::invalid_argument("TX, RX and generated sample rates must match.");
    tx_samples = sampleCount(chirp["tx_duration"].as<double>(), rate, "TX duration");
    rx_samples = sampleCount(chirp["rx_duration"].as<double>(), rate, "RX duration");
    pri_samples = sampleCount(chirp["pulse_rep_int"].as<double>(), rate, "PRI");
    const int64_t pulses = chirp["num_pulses"].as<int64_t>();
    const int64_t group = sync["pulses_per_frame"].as<int64_t>();
    const int64_t presums = chirp["num_presums"].as<int64_t>(1);
    if (pulses < 1 || pulses > 1000000 || group < 1 || group > pulses || presums < 1 || presums > pulses)
      throw std::invalid_argument("PoC requires 1..1000000 pulses and valid frame/stack counts.");
    total_pulses = static_cast<uint32_t>(pulses);
    pulses_per_frame = static_cast<uint32_t>(group);
    num_presums = static_cast<uint32_t>(presums);
    if (rx_samples < tx_samples || rx_samples > pri_samples)
      throw std::invalid_argument("Require TX duration <= RX duration <= PRI.");
    if (chirp["tr_on_lead"].as<double>(0) != 0 || chirp["tr_off_trail"].as<double>(0) != 0 ||
        chirp["tx_lead"].as<double>(0) != 0)
      throw std::invalid_argument("PoC uses zero GPIO lead/trail and zero tx_lead.");
    run_id = sync["run_id"].as<uint32_t>();
    phase_seed = sync["phase_seed"].as<uint32_t>(0);
    phase_dither = chirp["phase_dithering"].as<bool>(false);
    preamble_sps = sync["preamble_sps"].as<uint32_t>(4);
    header_sps = sync["header_sps"].as<uint32_t>(8);
    if (preamble_sps < 1 || preamble_sps > 64 || header_sps < 2 || header_sps > 64)
      throw std::invalid_argument("Invalid preamble/header samples per symbol.");
    preamble_samples = kPreambleChips * preamble_sps * kPreambleRepeats;
    payload_offset = preamble_samples + (kHeaderBytes * 8 + 1) * header_sps + 64;
    const uint64_t frame_size = payload_offset + uint64_t(pulses_per_frame) * pri_samples;
    if (frame_size > 16000000) throw std::invalid_argument("Frame exceeds the PoC's 128 MB buffer limit.");
    frame_samples = static_cast<uint32_t>(frame_size);
    if (double(frame_samples) / rate > 0.25)
      throw std::invalid_argument("PoC frames must be <= 250 ms; reduce pulses_per_frame or PRI.");
    frames = (total_pulses + pulses_per_frame - 1) / pulses_per_frame;
    start_time = sync["start_time"].as<double>(5);
    arm_timeout = sync["arm_timeout"].as<double>(120);
    quiet_seconds = sync["quiet_seconds"].as<double>(1.2);
    amplitude = sync["amplitude"].as<double>(0.35);
    if (!std::isfinite(start_time) || start_time < 2 || !std::isfinite(arm_timeout) || arm_timeout < 1 ||
        !std::isfinite(quiet_seconds) || quiet_seconds < 1.05 ||
        !std::isfinite(amplitude) || amplitude <= 0 || amplitude > 1)
      throw std::invalid_argument("Invalid start time, PPS timeouts or waveform amplitude.");
    lead_samples = sampleCount(sync["capture_lead"].as<double>(0.01), rate, "Capture lead");
    tail_samples = sampleCount(sync["capture_tail"].as<double>(0.01), rate, "Capture tail");
    if (double(lead_samples) / rate >= start_time - 0.5)
      throw std::invalid_argument("Capture lead leaves insufficient scheduling time.");
    frequency = rf["freq"].as<double>();
    bandwidth = config["GENERATE"]["chirp_bandwidth"].as<double>();
    chirp_length = config["GENERATE"]["chirp_length"].as<double>();
    if (!std::isfinite(frequency) || frequency <= 0 || !std::isfinite(bandwidth) || bandwidth <= 0 ||
        bandwidth > rate || !std::isfinite(chirp_length) || chirp_length <= 0 ||
        chirp_length > double(tx_samples) / rate)
      throw std::invalid_argument("Invalid RF frequency or chirp parameters.");
  }
  double frameTime(uint32_t index) const { return start_time + double(index) * frame_samples / rate; }
  double captureStart() const { return start_time - double(lead_samples) / rate; }
  uint64_t captureSamples() const { return uint64_t(frames) * frame_samples + lead_samples + tail_samples; }
};

/** Known PN block, repeated four times to allow carrier-offset estimation. */
inline std::vector<Sample> preamble(const Plan& plan) {
  std::vector<Sample> block;
  uint32_t state = 0x1aceb00c;
  for (uint32_t chip = 0; chip < kPreambleChips; ++chip) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    const float value = (state & 1) ? 1.0f : -1.0f;
    block.insert(block.end(), plan.preamble_sps, Sample(value, 0));
  }
  std::vector<Sample> result;
  for (uint32_t i = 0; i < kPreambleRepeats; ++i) result.insert(result.end(), block.begin(), block.end());
  return result;
}

/** IEEE CRC-32, matching Python zlib.crc32. */
inline uint32_t crc32(const std::vector<uint8_t>& bytes) {
  uint32_t crc = UINT32_MAX;
  for (const auto byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

/** Append integers/floats in an explicit little-endian, padding-free wire format. */
inline void appendInt(std::vector<uint8_t>& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
inline void appendDouble(std::vector<uint8_t>& bytes, double value) {
  static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559, "IEEE binary64 required");
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  appendInt(bytes, bits, sizeof(bits));
}

/** Metadata needed to identify a run/frame and reconstruct its radar pulses. */
inline std::vector<uint8_t> header(const Plan& p, uint32_t frame_index) {
  const uint32_t first = frame_index * p.pulses_per_frame;
  std::vector<uint8_t> bytes{'O', 'R', 'C', 'A'};
  appendInt(bytes, 1, 2); // Protocol v1 specifies MT19937 uint32 -> float32 phase in radians.
  appendInt(bytes, p.phase_dither ? 1 : 0, 2);
  for (const uint32_t value : {p.run_id, p.phase_seed, frame_index, first,
       std::min(p.pulses_per_frame, p.total_pulses - first), p.total_pulses,
       p.tx_samples, p.rx_samples, p.pri_samples, p.rate, p.frame_samples,
       p.preamble_samples, p.header_sps, p.payload_offset, p.num_presums}) appendInt(bytes, value, 4);
  for (const double value : {p.frequency, p.frameTime(frame_index), p.bandwidth, p.chirp_length}) appendDouble(bytes, value);
  appendInt(bytes, crc32(bytes), 4);
  if (bytes.size() != kHeaderBytes) throw std::logic_error("Unexpected PoC wire header length.");
  return bytes;
}

/** Load one cached complex-float waveform and reject a stale length or invalid samples. */
inline std::vector<Sample> loadChirp(const YAML::Node& config, const Plan& plan,
                                    const std::filesystem::path& root) {
  const auto path = root / config["FILES"]["output_dir"].as<std::string>("data") /
      config["FILES"]["chirp_loc"].as<std::string>();
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  const auto bytes = std::streamsize(plan.tx_samples * sizeof(Sample));
  if (!input || input.tellg() != bytes) throw std::runtime_error("Prepare the PoC chirp first: " + path.string());
  std::vector<Sample> result(plan.tx_samples);
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(result.data()), bytes)) throw std::runtime_error("Chirp read failed.");
  for (const auto value : result) {
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag()) || std::abs(value) > 1.001f)
      throw std::runtime_error("PoC chirp samples must be finite with magnitude <= 1.");
  }
  return result;
}

/** Construct one bounded frame: undithered preamble/header, guard, and pulsed chirps. */
inline std::vector<Sample> makeFrame(const Plan& p, uint32_t index,
                                    const std::vector<Sample>& chirp, std::mt19937& phases) {
  std::vector<Sample> frame(p.frame_samples, Sample(0, 0));
  const auto sync = preamble(p);
  const float amplitude = static_cast<float>(p.amplitude);
  for (size_t i = 0; i < sync.size(); ++i) frame[i] = amplitude * sync[i];
  size_t position = sync.size();
  float symbol = 1;
  const auto appendSymbol = [&](float value) {
    for (uint32_t i = 0; i < p.header_sps; ++i) frame[position++] = Sample(amplitude * value, 0);
  };
  appendSymbol(symbol);
  for (const auto byte : header(p, index)) {
    for (int bit = 7; bit >= 0; --bit) {
      if ((byte >> bit) & 1) symbol = -symbol;
      appendSymbol(symbol);
    }
  }
  const uint32_t count = std::min(p.pulses_per_frame, p.total_pulses - index * p.pulses_per_frame);
  for (uint32_t pulse = 0; pulse < count; ++pulse) {
    const Sample rotation = p.phase_dither ? std::polar(1.0f, static_cast<float>(phases())) : Sample(1, 0);
    const size_t offset = p.payload_offset + size_t(pulse) * p.pri_samples;
    for (size_t i = 0; i < chirp.size(); ++i) frame[offset + i] = amplitude * rotation * chirp[i];
  }
  return frame;
}
} // namespace poc
