/** @file Validate the existing experiment YAML and fingerprint its actual waveform. */
#pragma once
#include "protocol.hpp"
#include <yaml-cpp/yaml.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace bistatic {
namespace fs = std::filesystem;

inline double positive(const YAML::Node& node, const std::string& name) {
  double value = node.as<double>();
  if (!std::isfinite(value) || value <= 0) throw std::invalid_argument(name + " must be positive and finite");
  return value;
}

/** Read required text before opening hardware; empty device arguments permit discovery. */
inline std::string requiredText(const YAML::Node& node, const std::string& name, bool allow_empty = false) {
  if (!node || !node.IsScalar()) throw std::invalid_argument(name + " must be a string");
  auto value = node.as<std::string>();
  if (!allow_empty && value.find_first_not_of(" \t\r\n") == std::string::npos)
    throw std::invalid_argument(name + " must not be empty");
  return value;
}

/** Preserve fc32 IQ, optionally averaging consecutive, complete pulse windows. */
struct Config {
  YAML::Node yaml;
  double rate, clock_rate, pri, offset, tx_lead, frequency;
  std::string device_args, subdev, wire_format, antenna, tuning_args;
  double gain, lo_offset, bandwidth;
  double threshold = 0.85, tolerance = 0.001, max_ppm = 200, control_gain = 0;
  uint64_t pulses, presums, rx_samples, tx_samples, fingerprint = 14695981039346656037ull;
  const bool transmitting;
  bool dither;
  std::vector<Sample> chirp;

  Config(const fs::path& filename, const fs::path& repository, const std::string& role,
         const std::string& override_args = "")
      : yaml(YAML::LoadFile(filename.string())), transmitting(role == "tx") {
    if (role != "tx" && role != "rx") throw std::invalid_argument("Unknown radio role");
    auto gen = yaml["GENERATE"], device = yaml["DEVICE"], rf = yaml["RF0"], c = yaml["CHIRP"];
    device_args = override_args.empty() ? requiredText(device["device_args"], "DEVICE.device_args", true) : override_args;
    subdev = requiredText(device["subdev"], "DEVICE.subdev");
    wire_format = requiredText(device["otw_format"], "DEVICE.otw_format");
    if (wire_format != "sc16" && wire_format != "sc12" && wire_format != "sc8")
      throw std::invalid_argument("DEVICE.otw_format must be sc16, sc12 or sc8 in B2xx mode");
    antenna = requiredText(rf[transmitting ? "tx_ant" : "rx_ant"], transmitting ? "RF0.tx_ant" : "RF0.rx_ant");
    tuning_args = rf["tuning_args"] ? requiredText(rf["tuning_args"], "RF0.tuning_args", true) : "";
    rate = positive(gen["sample_rate"], "GENERATE.sample_rate");
    clock_rate = positive(device["clk_rate"], "DEVICE.clk_rate");
    frequency = positive(rf["freq"], "RF0.freq");
    if (rate > 56e6 || clock_rate < 5e6 || clock_rate > 56e6 || frequency < 70e6 || frequency > 6e9)
      throw std::invalid_argument("B2xx limits: sample rate <=56 MS/s, master clock 5..56 MHz, RF 70 MHz..6 GHz");
    pri = positive(c["pulse_rep_int"], "CHIRP.pulse_rep_int");
    offset = positive(c["time_offset"], "CHIRP.time_offset");
    tx_lead = c["tx_lead"].as<double>();
    dither = c["phase_dithering"].as<bool>(false);
    auto count = c["num_pulses"].as<int64_t>();
    if (count <= 0) throw std::invalid_argument("Each cycle needs CHIRP.num_pulses > 0; use --loops for repetition");
    pulses = static_cast<uint64_t>(count);
    auto samples = [this](const YAML::Node& value, const std::string& name) {
      double exact = positive(value, name) * rate;
      if (exact > 1e7 || std::abs(exact - std::round(exact)) > 1e-5) throw std::invalid_argument(name + " must give an integral sample count <= 10 million");
      auto result = static_cast<uint64_t>(std::llround(exact));
      if (!result) throw std::invalid_argument(name + " is shorter than one sample");
      return result;
    };
    tx_samples = samples(c["tx_duration"], "CHIRP.tx_duration");
    rx_samples = samples(c["rx_duration"], "CHIRP.rx_duration");
    if (device["cpu_format"].as<std::string>() != "fc32" || device[transmitting ? "tx_channels" : "rx_channels"].as<std::string>() != "0")
      throw std::invalid_argument("Bistatic mode needs one active channel (0) and cpu_format fc32");
    if (device["clk_ref"].as<std::string>() != "internal") throw std::invalid_argument("Shared-PPS mode uses clk_ref: internal and a separate external PPS time source");
    if (transmitting && !rf["transmit"].as<bool>(true)) throw std::invalid_argument("RF0.transmit must be true on the TX station");
    gain = rf[transmitting ? "tx_gain" : "rx_gain"].as<double>();
    lo_offset = rf["lo_offset"].as<double>(0);
    bandwidth = rf["bw"].as<double>(0);
    if (!std::isfinite(gain)) throw std::invalid_argument("Active RF gain must be finite");
    if (!std::isfinite(lo_offset)) throw std::invalid_argument("RF0.lo_offset must be finite");
    if (!std::isfinite(bandwidth) || bandwidth < 0) throw std::invalid_argument("RF0.bw must be finite and nonnegative");
    if (!std::isfinite(rf[transmitting ? "tx_rate" : "rx_rate"].as<double>()) ||
        std::abs(rf[transmitting ? "tx_rate" : "rx_rate"].as<double>() - rate) > 0.01)
      throw std::invalid_argument("GENERATE and the active RF0 sample rate must agree");
    auto averaging = c["num_presums"].as<int64_t>(1);
    if (averaging <= 0 || uint64_t(averaging) > pulses || pulses % uint64_t(averaging))
      throw std::invalid_argument("CHIRP.num_presums must be positive and divide CHIRP.num_pulses exactly");
    presums = static_cast<uint64_t>(averaging);
    if (!transmitting && yaml["FILES"]["max_chirps_per_file"].as<int>(-1) != -1)
      throw std::invalid_argument("Set max_chirps_per_file: -1; this mode writes one IQ file per cycle");
    if (yaml["GPIO"]["pwr_amp_pin"].as<int>(-1) != -1 || c["tr_on_lead"].as<double>(0) != 0 || c["tr_off_trail"].as<double>(0) != 0)
      throw std::invalid_argument("External amplifier GPIO gating is not implemented in bistatic mode");
    if (!std::isfinite(tx_lead) || offset - tx_lead < 0.25 || offset < 0.25 || tx_lead < 0 ||
        double(tx_samples) / rate >= pri || double(rx_samples) / rate >= pri)
      throw std::invalid_argument("Invalid pulse timing; leave >=0.25 seconds to arm after PPS, and keep pulse windows shorter than PRI");
    if (offset + double(pulses) * pri > 60) throw std::invalid_argument("Limit each cycle to 60 seconds; repeat with --loops");
    if (std::abs(clock_rate / kControlRate - std::round(clock_rate / kControlRate)) > 1e-8 ||
        std::abs(clock_rate / rate - std::round(clock_rate / rate)) > 1e-8)
      throw std::invalid_argument("Master clock must be an integer multiple of both radar rate and the 1 MS/s control rate");
    auto sync = yaml["BISTATIC"];
    if (sync) {
      threshold = sync["preamble_threshold"].as<double>(threshold);
      tolerance = sync["pps_phase_tolerance"].as<double>(tolerance);
      max_ppm = sync["max_clock_error_ppm"].as<double>(max_ppm);
      if (transmitting) control_gain = sync["control_tx_gain"].as<double>(control_gain);
    }
    if (!std::isfinite(threshold) || threshold < 0.6 || threshold > 1 || !std::isfinite(tolerance) || tolerance <= 0 || tolerance > 0.1 ||
        !std::isfinite(max_ppm) || max_ppm <= 0 || max_ppm > 1000 || !std::isfinite(control_gain))
      throw std::invalid_argument("Invalid BISTATIC control parameters");
    auto output_dir = repository / yaml["FILES"]["output_dir"].as<std::string>("data");
    fs::path waveform = output_dir / requiredText(yaml["FILES"]["chirp_loc"], "FILES.chirp_loc");
    auto generated_waveform = output_dir / requiredText(gen["out_file"], "GENERATE.out_file");
    if (fs::weakly_canonical(waveform) != fs::weakly_canonical(generated_waveform))
      throw std::invalid_argument("GENERATE.out_file and FILES.chirp_loc must identify the same waveform; refusing a stale chirp");
    if (!fs::is_regular_file(waveform) || fs::file_size(waveform) != tx_samples * sizeof(Sample))
      throw std::invalid_argument("Missing or wrong-size waveform; run --prepare after changing configuration");
    chirp.resize(tx_samples);
    std::ifstream source(waveform, std::ios::binary);
    source.read(reinterpret_cast<char*>(chirp.data()), static_cast<std::streamsize>(tx_samples * sizeof(Sample)));
    if (!source) throw std::runtime_error("Cannot read waveform");
    for (auto value : chirp) if (!std::isfinite(value.real()) || !std::isfinite(value.imag()) || std::abs(value) > 1.001f)
      throw std::invalid_argument("Waveform samples must be finite with amplitude <= 1");
    // Canonical, locale-independent experiment fields; device serial and gains can differ.
    std::vector<uint8_t> identity;
    for (uint64_t value : {uint64_t(std::llround(rate)), uint64_t(std::llround(clock_rate)), pulses, presums, rx_samples, tx_samples,
                           uint64_t(std::llround(pri * 1e12)), uint64_t(std::llround(offset * 1e12)),
                           uint64_t(std::llround(tx_lead * 1e12)), uint64_t(std::llround(frequency)), uint64_t(dither)}) appendInt(identity, value, 8);
    auto hash = [this](uint8_t value) { fingerprint = (fingerprint ^ value) * 1099511628211ull; };
    for (auto byte : identity) hash(byte);
    // IEEE-754 float32 encoded in little-endian order for identical ARM/x86 fingerprints.
    for (auto sample : chirp) for (float value : {sample.real(), sample.imag()}) {
      uint32_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      for (unsigned shift = 0; shift < 32; shift += 8) hash(uint8_t(bits >> shift));
    }
  }
};
}  // namespace bistatic
