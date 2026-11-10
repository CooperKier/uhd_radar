/** @file UHD transport, bounded control buffering, and configuration changes between cycles. */
#pragma once
#include "config.hpp"
#include "pps.hpp"
#include <uhd/types/tune_request.hpp>
#include <functional>
#include <iostream>
#include <sstream>

namespace bistatic {
struct Received { Packet packet; double time; double carrier_hz; double score; };

class Radio {
  const Config& config;
  const bool transmitting;
  std::vector<Sample> pending;
  double pending_time = 0;
  bool listening = false;
  DetectionStats detection_stats;
  uint64_t control_samples = 0, control_timeouts = 0, control_overflows = 0, control_gaps = 0, near_fullscale = 0;
  double peak_power = 0;
  Steady::time_point control_report = Steady::now();

  /** Report sample flow and decoder progress every five wall-clock seconds. */
  void reportControl() {
    if (Steady::now() - control_report < std::chrono::seconds(5)) return;
    std::ostringstream message;
    message << "[SYNC RX] samples=" << control_samples << " timeouts=" << control_timeouts
            << " overflows=" << control_overflows << " timestamp_gaps=" << control_gaps
            << " peak_dbfs=" << 10 * std::log10(std::max(peak_power, 1e-20))
            << " near_fullscale=" << near_fullscale << " best_corr=" << detection_stats.best_score
            << " preamble_hits=" << detection_stats.preamble_hits
            << " invalid_headers=" << detection_stats.invalid_headers
            << " decoded=" << detection_stats.decoded_packets;
    std::cout << message.str() << std::endl;
    detection_stats = {};
    control_samples = control_timeouts = control_overflows = control_gaps = near_fullscale = 0;
    peak_power = 0;
    control_report = Steady::now();
  }

  /** Record normalized IQ levels; near-full-scale samples suggest possible clipping. */
  void observeControl(const std::vector<Sample>& buffer, size_t count) {
    control_samples += count;
    for (size_t i = 0; i < count; ++i) {
      peak_power = std::max(peak_power, double(std::norm(buffer[i])));
      if (std::abs(buffer[i].real()) >= 0.98f || std::abs(buffer[i].imag()) >= 0.98f) ++near_fullscale;
    }
  }

 public:
  uhd::usrp::multi_usrp::sptr device;
  uhd::tx_streamer::sptr tx;
  uhd::rx_streamer::sptr rx;

  Radio(const Config& settings, const std::string& override_args, const std::string& role)
      : config(settings), transmitting(role == "tx") {
    if (role != "tx" && role != "rx") throw std::invalid_argument("Unknown radio role");
    std::string arguments = override_args.empty() ? config.yaml["DEVICE"]["device_args"].as<std::string>() : override_args;
    device = uhd::usrp::multi_usrp::make(arguments);
    // Master clock selection precedes all PPS synchronization.
    device->set_master_clock_rate(config.clock_rate);
    device->set_clock_source("internal");
    device->set_time_source("external");
    auto subdev = config.yaml["DEVICE"]["subdev"].as<std::string>();
    auto rf = config.yaml["RF0"];
    uhd::tune_request_t tune(config.frequency, rf["lo_offset"].as<double>(0));
    tune.args = uhd::device_addr_t(rf["tuning_args"].as<std::string>(""));
    double bandwidth = rf["bw"].as<double>(0);
    if (transmitting) {
      device->set_tx_subdev_spec(subdev);
      device->set_tx_antenna(rf["tx_ant"].as<std::string>());
      device->set_tx_freq(tune);
      if (bandwidth > 0) device->set_tx_bandwidth(bandwidth);
    } else {
      device->set_rx_subdev_spec(subdev);
      device->set_rx_antenna(rf["rx_ant"].as<std::string>());
      device->set_rx_freq(tune);
      if (bandwidth > 0) device->set_rx_bandwidth(bandwidth);
    }
    // Local epochs may differ; RX maps the TX announcement to its own PPS history.
    device->set_time_next_pps(uhd::time_spec_t(0.0));
    control();
  }

  ~Radio() {
    try {
      if (tx) {
        Sample zero(0, 0);
        uhd::tx_metadata_t metadata;
        metadata.end_of_burst = true;
        tx->send(&zero, 0, metadata, 0.1);
      }
    } catch (...) {}
    try { stopReceive(); } catch (...) {}
  }

  void makeStreams(double rate, const std::string& wire) {
    stopReceive();
    rx.reset();
    tx.reset();
    uhd::stream_args_t args("fc32", wire);
    args.channels = {0};
    double actual;
    if (transmitting) {
      device->set_tx_rate(rate);
      actual = device->get_tx_rate();
      tx = device->get_tx_stream(args);
    } else {
      device->set_rx_rate(rate);
      actual = device->get_rx_rate();
      rx = device->get_rx_stream(args);
    }
    if (std::abs(actual - rate) > 0.01 || std::abs(device->get_master_clock_rate() - config.clock_rate) > 0.01)
      throw std::runtime_error("UHD coerced a requested sample/master clock rate");
    pending.clear();
  }

  void control() {
    makeStreams(kControlRate, "sc16");
    if (transmitting) device->set_tx_gain(config.control_gain);
    else device->set_rx_gain(config.yaml["RF0"]["rx_gain"].as<double>());
    std::cout << "[CONTROL] role=" << (transmitting ? "tx" : "rx")
              << " center_hz=" << std::to_string(transmitting ? device->get_tx_freq() : device->get_rx_freq())
              << " rate_sps=" << kControlRate
              << " gain_db=" << (transmitting ? device->get_tx_gain() : device->get_rx_gain())
              << " antenna=" << (transmitting ? device->get_tx_antenna() : device->get_rx_antenna()) << std::endl;
  }

  void radar() {
    makeStreams(config.rate, config.yaml["DEVICE"]["otw_format"].as<std::string>());
    if (transmitting) device->set_tx_gain(config.yaml["RF0"]["tx_gain"].as<double>());
    else device->set_rx_gain(config.yaml["RF0"]["rx_gain"].as<double>());
  }

  void startReceive() {
    if (!rx) throw std::logic_error("TX-only station cannot receive");
    if (listening) return;
    uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    command.stream_now = true;
    rx->issue_stream_cmd(command);
    listening = true;
    pending.clear();
    detection_stats = {};
    control_samples = control_timeouts = control_overflows = control_gaps = near_fullscale = 0;
    peak_power = 0;
    control_report = Steady::now();
    std::cout << "[SYNC RX] Listening for RF packets; threshold=" << config.threshold
              << " PPS_tolerance_ms=" << config.tolerance * 1000 << std::endl;
  }

  /** Stop and drain before changing rates or issuing a new timed acquisition. */
  void stopReceive() {
    if (!rx) return;
    uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
    rx->issue_stream_cmd(command);
    listening = false;
    std::vector<Sample> discard(16384);
    uhd::rx_metadata_t metadata;
    auto deadline = Steady::now() + std::chrono::milliseconds(500);
    while (Steady::now() < deadline && rx->recv(discard.data(), discard.size(), metadata, 0.01, false)) {}
    pending.clear();
  }

  /** Local UHD burst-completion events are driver status, never RF messages from RX. */
  size_t drainTransmit() {
    if (!tx) throw std::logic_error("RX-only station cannot transmit");
    size_t completed_bursts = 0;
    uhd::async_metadata_t metadata;
    while (tx->recv_async_msg(metadata, 0)) {
      if (metadata.event_code != uhd::async_metadata_t::EVENT_CODE_BURST_ACK)
        throw std::runtime_error("UHD TX asynchronous error " + std::to_string(unsigned(metadata.event_code)));
      ++completed_bursts;
    }
    return completed_bursts;
  }

  void sendControl(const Packet& packet, double time, PpsClock& clock) {
    if (!tx) throw std::logic_error("RX-only station cannot transmit");
    clock.check();
    if (device->get_time_now().get_real_secs() + 0.03 > time) throw std::runtime_error("RF control scheduling deadline missed");
    drainTransmit();
    auto waveform = modulate(packet);
    uhd::tx_metadata_t metadata;
    metadata.start_of_burst = metadata.end_of_burst = metadata.has_time_spec = true;
    metadata.time_spec = uhd::time_spec_t(time);
    if (tx->send(waveform.data(), waveform.size(), metadata, 0.5) != waveform.size()) throw std::runtime_error("Short RF control transmission");
    auto deadline = Steady::now() + std::chrono::seconds(2);
    while (Steady::now() < deadline) {
      clock.check();
      if (drainTransmit()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("Missing RF sync burst completion from UHD");
  }

  /** Hardware RX timestamps are retained across USB buffer boundaries. */
  std::optional<Received> receiveUntil(double deadline, PpsClock& clock,
                                       const std::function<bool(const Received&)>& accept) {
    startReceive();
    std::vector<Sample> buffer(16384);
    while (!interrupted && device->get_time_now().get_real_secs() < deadline) {
      clock.check();
      reportControl();
      uhd::rx_metadata_t metadata;
      size_t count = rx->recv(buffer.data(), buffer.size(), metadata, 0.1, false);
      if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
        ++control_timeouts;
        continue;
      }
      if (metadata.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
        ++control_overflows;
        pending.clear();
        continue;  // Control messages can be retried; never decode across a gap.
      }
      if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) throw std::runtime_error("RF control receive: " + metadata.strerror());
      if (!count) continue;
      observeControl(buffer, count);
      if (!metadata.has_time_spec) throw std::runtime_error("Missing RF packet hardware timestamp");
      double first = metadata.time_spec.get_real_secs();
      if (!pending.empty() && std::abs(first - pending_time - double(pending.size()) / kControlRate) > 2 / kControlRate) {
        ++control_gaps;
        pending.clear();
      }
      if (pending.empty()) pending_time = first;
      pending.insert(pending.end(), buffer.begin(), buffer.begin() + count);
      while (auto decoded = detect(pending, config.threshold, &detection_stats)) {
        Received found{decoded->packet, pending_time + double(decoded->sample) / kControlRate, decoded->carrier_hz, decoded->score};
        std::cout << "[SYNC RX] Decoded cycle=" << found.packet.cycle
                  << " copy=" << unsigned(found.packet.repetition) + 1
                  << " correlation=" << found.score << " carrier_offset_hz=" << found.carrier_hz << std::endl;
        size_t consumed = decoded->sample + packetSamples();
        pending.erase(pending.begin(), pending.begin() + consumed);
        pending_time += double(consumed) / kControlRate;
        if (accept(found)) return found;
      }
      // Preserve an incomplete packet, with a bounded overlap for the next USB block.
      if (pending.size() > packetSamples()) {
        size_t consumed = pending.size() - packetSamples();
        pending.erase(pending.begin(), pending.begin() + consumed);
        pending_time += double(consumed) / kControlRate;
      }
    }
    clock.check();
    return std::nullopt;
  }
};
}  // namespace bistatic
