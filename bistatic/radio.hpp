/** @file UHD transport, bounded control buffering, and configuration changes between cycles. */
#pragma once
#include "config.hpp"
#include <uhd/usrp/multi_usrp.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <uhd/types/tune_request.hpp>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace bistatic {
using Steady = std::chrono::steady_clock;
extern std::atomic<bool> interrupted;

struct Edge { uint64_t sequence; double time; };

/** Local edge numbers deliberately need not agree between the two stations. */
class PpsClock {
  uhd::usrp::multi_usrp::sptr radio;
  double max_ppm;
  std::thread worker;
  std::atomic<bool> stop{false};
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::deque<Edge> edges;
  std::deque<double> periods;
  std::string failure;

  void observe() {
    try {
      double previous = radio->get_time_last_pps().get_real_secs();
      auto last_seen = Steady::now();
      while (!stop && !interrupted) {
        double current = radio->get_time_last_pps().get_real_secs();
        if (current != previous) {
          std::lock_guard<std::mutex> lock(mutex);
          if (!edges.empty()) {
            double period = current - previous;
            if (!std::isfinite(period) || std::abs(period - 1) * 1e6 > max_ppm)
              throw std::runtime_error("PPS jump or clock-rate error exceeds configured limit");
            periods.push_back(period);
            if (periods.size() > 8) periods.pop_front();
          }
          edges.push_back({edges.empty() ? 1 : edges.back().sequence + 1, current});
          if (edges.size() > 128) edges.pop_front();
          previous = current;
          last_seen = Steady::now();
          changed.notify_all();
        }
        if (Steady::now() - last_seen > std::chrono::milliseconds(2500)) throw std::runtime_error("Missing external 1 Hz PPS");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex);
      failure = error.what();
      changed.notify_all();
    }
  }

 public:
  PpsClock(uhd::usrp::multi_usrp::sptr device, double limit) : radio(device), max_ppm(limit), worker() {
    worker = std::thread([this] { observe(); });
  }
  ~PpsClock() { stop = true; if (worker.joinable()) worker.join(); }

  void check() const {
    if (interrupted) throw std::runtime_error("Interrupted");
    std::lock_guard<std::mutex> lock(mutex);
    if (!failure.empty()) throw std::runtime_error(failure);
  }

  /** Estimate radio seconds per physical second from recent measured PPS intervals. */
  double ratio() const {
    check();
    std::lock_guard<std::mutex> lock(mutex);
    if (periods.size() < 2) throw std::runtime_error("PPS clock has not acquired three edges yet");
    double sum = 0;
    for (double period : periods) sum += period;
    return sum / periods.size();
  }

  Edge waitEdge(uint64_t sequence, double timeout = 5) {
    auto deadline = Steady::now() + std::chrono::duration<double>(timeout);
    std::unique_lock<std::mutex> lock(mutex);
    while (!interrupted && failure.empty() && (edges.empty() || edges.back().sequence < sequence)) {
      changed.wait_for(lock, std::chrono::milliseconds(20));
      if (Steady::now() >= deadline) throw std::runtime_error("PPS edge wait timed out");
    }
    if (interrupted) throw std::runtime_error("Interrupted");
    if (!failure.empty()) throw std::runtime_error(failure);
    for (const auto& edge : edges) if (edge.sequence == sequence) return edge;
    throw std::runtime_error("Requested PPS edge is outside the retained history");
  }

  Edge latest() const {
    check();
    std::lock_guard<std::mutex> lock(mutex);
    if (edges.empty()) throw std::runtime_error("No PPS edge acquired");
    return edges.back();
  }

  /** Associate an RF packet timestamp with the physical PPS edge before it. */
  Edge before(double timestamp) const {
    check();
    std::lock_guard<std::mutex> lock(mutex);
    for (auto it = edges.rbegin(); it != edges.rend(); ++it) if (it->time <= timestamp) return *it;
    throw std::runtime_error("RF timestamp predates the PPS history");
  }
};

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
            << " candidate_cfo_hz=" << detection_stats.best_carrier_hz
            << " preamble_hits=" << detection_stats.preamble_hits
            << " invalid_headers=" << detection_stats.invalid_headers
            << " bad_headers=" << detection_stats.bad_headers
            << " protocol_mismatches=" << detection_stats.protocol_mismatches
            << " crc_failures=" << detection_stats.crc_failures
            << " decoded=" << detection_stats.decoded_packets;
    if (detection_stats.preamble_hits) {
      message << " candidate_header=" << std::hex << std::setfill('0');
      for (auto byte : detection_stats.best_header) message << std::setw(2) << unsigned(byte);
    }
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

  explicit Radio(const Config& settings) : config(settings), transmitting(settings.transmitting) {
    device = uhd::usrp::multi_usrp::make(config.device_args);
    // Master clock selection precedes all PPS synchronization.
    device->set_master_clock_rate(config.clock_rate);
    device->set_clock_source("internal");
    device->set_time_source("external");
    uhd::tune_request_t tune(config.frequency, config.lo_offset);
    tune.args = uhd::device_addr_t(config.tuning_args);
    if (transmitting) {
      device->set_tx_subdev_spec(config.subdev);
      device->set_tx_antenna(config.antenna);
      device->set_tx_freq(tune);
      if (config.bandwidth > 0) device->set_tx_bandwidth(config.bandwidth);
    } else {
      device->set_rx_subdev_spec(config.subdev);
      device->set_rx_antenna(config.antenna);
      device->set_rx_freq(tune);
      if (config.bandwidth > 0) device->set_rx_bandwidth(config.bandwidth);
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
    else device->set_rx_gain(config.gain);
    std::cout << "[CONTROL] role=" << (transmitting ? "tx" : "rx")
              << " center_hz=" << std::to_string(transmitting ? device->get_tx_freq() : device->get_rx_freq())
              << " rate_sps=" << kControlRate
              << " gain_db=" << (transmitting ? device->get_tx_gain() : device->get_rx_gain())
              << " antenna=" << (transmitting ? device->get_tx_antenna() : device->get_rx_antenna()) << std::endl;
  }

  void radar() {
    makeStreams(config.rate, config.wire_format);
    if (transmitting) device->set_tx_gain(config.gain);
    else device->set_rx_gain(config.gain);
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
  Received receiveControl(PpsClock& clock, const std::function<bool(const Received&)>& accept) {
    startReceive();
    std::vector<Sample> buffer(16384);
    while (!interrupted) {
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
    throw std::runtime_error("Interrupted");
  }
};
}  // namespace bistatic
